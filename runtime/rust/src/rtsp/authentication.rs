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
#[derive(Clone)]
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
    /// For a 401 that is answered: the CSeq of the request sent again with
    /// the answer, whose response is the one that counts. CSeq advances only
    /// as a request is sent, and nothing else is sent before it, so it is the
    /// 401's request's CSeq plus one.
    pub answered_by: Option<u32>,
    /// For a reply to a Digest answer: whether its `Authentication-Info`
    /// proved the server holds the password. A server that sends no proof is
    /// taken at its word; a proof that does not verify fails the connection.
    pub server_proof: Option<bool>,
}

/// A request as the caller made it, kept while it is in flight so a 401 to it
/// can be answered by sending it again. Only a Digest connection keeps one.
pub(super) struct Sent {
    pub(super) method: String,
    pub(super) uri: String,
    headers: Vec<(String, Zeroizing<String>)>,
    pub(super) body: Vec<u8>,
    /// Already sent again: a 401 to it is the response.
    answered: bool,
    /// One generation for this exchange, including its challenge answer.
    credential: Credential,
}

/// A request as it is sent: its headers with the credential's
/// `Authorization`, and for Digest the request kept and its answer's proof.
pub(super) struct Authorized {
    headers: Vec<(String, Zeroizing<String>)>,
    kept: Option<(Sent, Option<ServerProof>)>,
}
impl Authorized {
    pub(super) fn headers(&self) -> Vec<(&str, &str)> {
        self.headers
            .iter()
            .map(|(name, value)| (name.as_str(), value.as_str()))
            .collect()
    }
    /// The request kept to be sent again: its method, URI and body.
    pub(super) fn kept(&self) -> Option<&Sent> {
        self.kept.as_ref().map(|(sent, _)| sent)
    }
}

struct InFlight {
    sent: Sent,
    sequence: u32,
    proof: Option<ServerProof>,
}

/// Supplies one credential generation at the start of each request's authentication
/// exchange. A challenge answer and its server proof keep that same generation.
pub type CredentialCallback = Box<dyn FnMut() -> Result<Credential, Error> + Send>;

enum Source {
    Fixed(Credential),
    Callback {
        acquire: CredentialCallback,
        basic: bool,
    },
}

pub(super) struct Authentication {
    source: Source,
    session: DigestSession,
    /// The Digest request in flight.
    in_flight: Option<InFlight>,
    /// The Digest request to send again once the writer is free, and the CSeq
    /// its 401 said it is sent as.
    due: Option<(Sent, u32)>,
    /// The request sent again is being written.
    pub(super) writing: bool,
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
            source: Source::Fixed(credential),
            // An RTSP owner holds every request and reply body whole.
            session: DigestSession::new(true),
            in_flight: None,
            due: None,
            writing: false,
        })
    }
    pub(super) fn callback(acquire: CredentialCallback, basic: bool) -> Self {
        Self {
            source: Source::Callback { acquire, basic },
            session: DigestSession::new(true),
            in_flight: None,
            due: None,
            writing: false,
        }
    }
    pub(super) fn acquires(&self) -> bool {
        matches!(self.source, Source::Callback { .. })
    }
    pub(super) fn is_basic(&self) -> bool {
        match &self.source {
            Source::Fixed(credential) => matches!(credential, Credential::Basic(_)),
            Source::Callback { basic, .. } => *basic,
        }
    }
    /// A Digest request is in flight: its final response is held natively.
    pub(super) fn holds(&self) -> bool {
        self.in_flight.is_some()
    }
    /// A request sent again is due or being written: nothing else is sent.
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
        let owned = || {
            headers
                .iter()
                .map(|(name, value)| ((*name).to_owned(), Zeroizing::new((*value).to_owned())))
                .collect::<Vec<_>>()
        };
        let credential = match &mut self.source {
            Source::Fixed(credential) => credential.clone(),
            Source::Callback { acquire, basic } => {
                let credential = acquire()?;
                if matches!(credential, Credential::Basic(_)) != *basic {
                    return Err(Error::AUTHENTICATION);
                }
                credential
            }
        };
        let fits = match &credential {
            Credential::Basic(value) => value.len() <= 65536,
            Credential::Digest { username, password } => {
                username.len() <= 65536 && password.len() <= 65536
            }
        };
        if !fits {
            return Err(Error::AUTHENTICATION);
        }
        if let Credential::Basic(value) = &credential {
            let mut headers = owned();
            headers.push(("Authorization".into(), value.clone()));
            return Ok(Authorized {
                headers,
                kept: None,
            });
        }
        self.digest(Sent {
            method: method.into(),
            uri: uri.into(),
            headers: owned(),
            body: body.to_vec(),
            answered: false,
            credential,
        })
    }
    /// `sent` with the Digest session's answer, once a challenge was adopted.
    fn digest(&mut self, sent: Sent) -> Result<Authorized, Error> {
        let Credential::Digest { username, password } = &sent.credential else {
            unreachable!("only a Digest connection answers a challenge");
        };
        let answer = self
            .session
            .respond(&PasswordParams {
                username,
                password,
                method: &sent.method,
                uri: &sent.uri,
                body: Some(&sent.body),
            })
            .map_err(|_| Error::AUTHENTICATION)?;
        let mut headers = sent.headers.clone();
        let proof = answer.map(|answer| {
            headers.push(("Authorization".into(), answer.authorization));
            answer.proof
        });
        Ok(Authorized {
            headers,
            kept: Some((sent, proof)),
        })
    }
    /// The request just begun as `sequence` is in flight. True when it is
    /// a Digest request, whose final response [`Self::received`] must accept
    /// before it changes any state.
    pub(super) fn dispatched(&mut self, request: Authorized, sequence: u32) -> bool {
        self.in_flight = request.kept.map(|(sent, proof)| InFlight {
            sent,
            sequence,
            proof,
        });
        self.in_flight.is_some()
    }
    /// Takes a complete response to the request in flight. A 401 offering a
    /// Digest challenge to a request not yet answered makes it due again and
    /// names the CSeq it will be sent as; a reply to a Digest answer must
    /// carry no proof that fails.
    pub(super) fn received(&mut self, message: &mut Message) -> Result<(), Error> {
        let Some(InFlight {
            sent,
            sequence,
            proof,
        }) = self.in_flight.take()
        else {
            return Ok(());
        };
        if message.status == 401 {
            // A later request answers the latest challenge, even when this
            // one is not sent again.
            let challenges = message
                .headers
                .iter()
                .filter(|(name, _)| name.eq_ignore_ascii_case(b"www-authenticate"))
                .filter_map(|(_, value)| std::str::from_utf8(value).ok());
            if self.session.challenged(challenges).is_err() || sent.answered {
                return Ok(());
            }
            let Some(next) = sequence.checked_add(1) else {
                return Ok(());
            };
            self.due = Some((
                Sent {
                    answered: true,
                    ..sent
                },
                next,
            ));
            message.authentication = Some(Observation {
                answered_by: Some(next),
                server_proof: None,
            });
            return Ok(());
        }
        let Some(proof) = proof else {
            return Ok(());
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
        proven.map(drop).map_err(|_| Error::AUTHENTICATION)
    }
    /// The request due again, answered now, and the CSeq it must be sent as.
    pub(super) fn take_due(&mut self) -> Option<Result<(Authorized, u32), Error>> {
        let (sent, sequence) = self.due.take()?;
        Some(self.digest(sent).map(|request| (request, sequence)))
    }
    /// The writer was occupied: the request stays due.
    pub(super) fn keep_due(&mut self, request: Authorized, sequence: u32) {
        let (sent, _) = request.kept.expect("a due request is kept");
        self.due = Some((sent, sequence));
    }
}

#[cfg(test)]
mod generation_tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    #[test]
    fn a_committed_generation_reaches_the_next_exchange() {
        let current = Arc::new(Mutex::new("Basic first".to_owned()));
        let values = current.clone();
        let mut auth = Authentication::callback(
            Box::new(move || {
                Ok(Credential::Basic(Zeroizing::new(
                    values.lock().unwrap().clone(),
                )))
            }),
            true,
        );
        let first = auth
            .request("OPTIONS", "rtsp://camera/test", &[], &[])
            .unwrap();
        assert_eq!(first.headers()[0].1, "Basic first");
        *current.lock().unwrap() = "Basic second".into();
        let next = auth
            .request("OPTIONS", "rtsp://camera/test", &[], &[])
            .unwrap();
        assert_eq!(next.headers()[0].1, "Basic second");
    }

    #[test]
    fn each_exchange_acquires_the_current_generation_and_its_answer_keeps_it() {
        let current = Arc::new(Mutex::new("first".to_owned()));
        let reads = Arc::new(Mutex::new(0));
        let (values, acquired) = (current.clone(), reads.clone());
        let mut auth = Authentication::callback(
            Box::new(move || {
                *acquired.lock().unwrap() += 1;
                Ok(Credential::Digest {
                    username: Zeroizing::new("user".into()),
                    password: Zeroizing::new(values.lock().unwrap().clone()),
                })
            }),
            false,
        );
        let first = auth
            .request("OPTIONS", "rtsp://camera/test", &[], &[])
            .unwrap();
        assert!(auth.dispatched(first, 1));
        *current.lock().unwrap() = "second".into();
        let mut challenge = Message {
            kind: 2,
            status: 401,
            channel: 0,
            version: 2,
            method: None,
            uri: None,
            reason: None,
            body: vec![],
            raw: vec![],
            authentication: None,
            headers: vec![(
                b"WWW-Authenticate".to_vec(),
                b"Digest realm=\"camera\",nonce=\"one\",algorithm=SHA-256,qop=\"auth\"".to_vec(),
            )],
        };
        auth.received(&mut challenge).unwrap();
        let (answer, _) = auth.take_due().unwrap().unwrap();
        let sent = answer.kept().unwrap();
        let Credential::Digest { password, .. } = &sent.credential else {
            panic!("Digest")
        };
        assert_eq!(password.as_str(), "first");
        assert_eq!(*reads.lock().unwrap(), 1);
        let next = auth
            .request("OPTIONS", "rtsp://camera/test", &[], &[])
            .unwrap();
        let Credential::Digest { password, .. } = &next.kept().unwrap().credential else {
            panic!("Digest")
        };
        assert_eq!(password.as_str(), "second");
        assert_eq!(*reads.lock().unwrap(), 2);
    }
}
