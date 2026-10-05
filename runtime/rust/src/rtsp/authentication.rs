//! A connection's credential, sent with every request this owner makes: a
//! Basic value from the first request, a Digest answer from the 401 that
//! asked for one on. http-auth's `DigestSession` keeps the Digest state, as
//! it does for Boundless's HTTP answers, so the connection's later requests
//! and keepalives reuse the adopted nonce without another 401. A 401 to a
//! request is answered once, by sending the request again with the answer,
//! as GStreamer's rtspsrc and curl do.
use super::{Error, Message};
use http_auth::{
    PasswordParams,
    digest::{DigestSession, ServerInfo, ServerProof},
};
use zeroize::Zeroizing;

/// What a connection authenticates its requests with.
pub enum Credential {
    /// A Basic `Authorization` value, sent with every request. RTSP 2.0
    /// carries it only over TLS (RFC 7826 section 19.1).
    Basic(Zeroizing<String>),
    /// A Digest username and password, answering the server's challenge.
    Digest {
        username: Zeroizing<String>,
        password: Zeroizing<String>,
    },
}

/// What authentication did with a response.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Observation {
    /// For a 401 that was answered: the CSeq of the request sent again with
    /// the answer, whose response is the one that counts.
    pub answered_by: Option<u32>,
    /// For a reply to a Digest answer: whether its `Authentication-Info`
    /// proved the server holds the password. A server that sends no proof is
    /// taken at its word; a proof that does not verify fails the connection.
    pub server_proof: Option<bool>,
}

/// A request as the caller made it, kept until its response to send it again
/// with the answer to a 401.
#[derive(Clone)]
pub(super) struct Sent {
    pub(super) method: String,
    pub(super) uri: String,
    headers: Vec<(String, Zeroizing<String>)>,
    pub(super) body: Vec<u8>,
    /// Already sent again: a 401 to it is the response.
    answered: bool,
}

/// A request as it is sent: the caller's, with the credential's
/// `Authorization`, and the proof that checks a reply to a Digest answer.
pub(super) struct Authorized {
    pub(super) sent: Sent,
    headers: Vec<(String, Zeroizing<String>)>,
    pub(super) proof: Option<ServerProof>,
}
impl Authorized {
    pub(super) fn headers(&self) -> Vec<(&str, &str)> {
        self.headers
            .iter()
            .map(|(name, value)| (name.as_str(), value.as_str()))
            .collect()
    }
}

pub(super) struct Authentication {
    credential: Credential,
    session: DigestSession,
    /// The request in flight, and the proof that checks a reply to its answer.
    sent: Option<(Sent, Option<ServerProof>)>,
    /// The request to send again once the writer is free, and its 401, which
    /// the caller receives when the request has been begun.
    due: Option<(Sent, Message)>,
    /// The 401 whose request has been begun again, for the caller's next read.
    pub(super) released: Option<Message>,
    /// The request sent again is being written.
    pub(super) writing: bool,
}

/// What a 401 or a reply did.
pub(super) enum Received {
    /// The caller receives the message now.
    Deliver,
    /// The request is to be sent again; its 401 waits for that.
    Due,
}

impl Authentication {
    pub(super) fn new(credential: Credential) -> Result<Self, Error> {
        let fits = |value: &str| value.len() <= 65536;
        if !match &credential {
            Credential::Basic(value) => fits(value),
            Credential::Digest { username, password } => fits(username) && fits(password),
        } {
            return Err(Error::AUTHENTICATION);
        }
        Ok(Self {
            credential,
            // An RTSP owner holds every request and reply body whole.
            session: DigestSession::new(true),
            sent: None,
            due: None,
            released: None,
            writing: false,
        })
    }
    pub(super) fn is_basic(&self) -> bool {
        matches!(self.credential, Credential::Basic(_))
    }
    /// Something sent by this owner itself is due or being written.
    pub(super) fn busy(&self) -> bool {
        self.due.is_some() || self.writing
    }
    /// The request a caller makes, as it is sent: its headers and the
    /// credential's `Authorization`, which the caller may not set itself.
    pub(super) fn request(
        &mut self,
        method: &str,
        uri: &str,
        headers: &[(&str, &str)],
        body: &[u8],
    ) -> Result<Authorized, Error> {
        if headers
            .iter()
            .any(|(name, _)| name.eq_ignore_ascii_case("authorization"))
        {
            return Err(Error::INVALID);
        }
        let sent = Sent {
            method: method.into(),
            uri: uri.into(),
            headers: headers
                .iter()
                .map(|(name, value)| ((*name).to_owned(), Zeroizing::new((*value).to_owned())))
                .collect(),
            body: body.to_vec(),
            answered: false,
        };
        self.authorize(sent)
    }
    fn authorize(&mut self, sent: Sent) -> Result<Authorized, Error> {
        let mut headers = sent.headers.clone();
        let (value, proof) = match &self.credential {
            Credential::Basic(value) => (Some(value.clone()), None),
            Credential::Digest { username, password } => {
                match self
                    .session
                    .respond(&PasswordParams {
                        username,
                        password,
                        method: &sent.method,
                        uri: &sent.uri,
                        body: Some(&sent.body),
                    })
                    .map_err(|_| Error::AUTHENTICATION)?
                {
                    Some(response) => (Some(response.authorization), Some(response.proof)),
                    None => (None, None),
                }
            }
        };
        if let Some(value) = value {
            headers.push(("Authorization".into(), value));
        }
        Ok(Authorized {
            sent,
            headers,
            proof,
        })
    }
    /// The request just begun is in flight.
    pub(super) fn dispatched(&mut self, request: Authorized) {
        self.sent = Some((request.sent, request.proof));
    }
    /// Takes a complete response to the request in flight. A 401 offering a
    /// Digest challenge to a request not yet answered makes it due again;
    /// a reply to a Digest answer must carry no proof that fails.
    pub(super) fn received(&mut self, message: &mut Message) -> Result<Received, Error> {
        let Some((sent, proof)) = self.sent.take() else {
            return Ok(Received::Deliver);
        };
        if message.status == 401 {
            if self.is_basic() {
                return Ok(Received::Deliver);
            }
            // A later request answers the latest challenge, even when this
            // one is not sent again.
            let challenges = message
                .headers
                .iter()
                .filter(|(name, _)| name.eq_ignore_ascii_case(b"www-authenticate"))
                .filter_map(|(_, value)| std::str::from_utf8(value).ok());
            if self.session.challenged(challenges).is_err() || sent.answered {
                return Ok(Received::Deliver);
            }
            self.due = Some((
                Sent {
                    answered: true,
                    ..sent
                },
                std::mem::take(message),
            ));
            return Ok(Received::Due);
        }
        let Some(proof) = proof else {
            return Ok(Received::Deliver);
        };
        // Every line of the field; http-auth reads them as one list.
        let info = message
            .headers
            .iter()
            .filter(|(name, _)| name.eq_ignore_ascii_case(b"authentication-info"))
            .map(|(_, value)| value.as_slice());
        let proven = self.session.replied(&proof, info, &message.body);
        message.authentication = Some(Observation {
            answered_by: None,
            server_proof: Some(matches!(proven, Ok(ServerInfo::Proven { .. }))),
        });
        proven
            .map(|_| Received::Deliver)
            .map_err(|_| Error::AUTHENTICATION)
    }
    /// The request due again, as it is sent now, with its 401.
    pub(super) fn take_due(&mut self) -> Option<(Result<Authorized, Error>, Message)> {
        let (sent, message) = self.due.take()?;
        Some((self.authorize(sent), message))
    }
    /// The writer was occupied: the request stays due.
    pub(super) fn keep_due(&mut self, sent: Sent, message: Message) {
        self.due = Some((sent, message));
    }
}
