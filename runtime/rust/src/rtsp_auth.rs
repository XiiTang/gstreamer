//! Explicit RTSP challenge contexts. All password material and response proofs
//! stay private to the media library; observations contain only safe metadata.
use crate::Error;
use http_auth::{
    BasicClient, DigestClient, PasswordParams,
    digest::{Algorithm as Hash, Qop as HttpQop, ServerProof},
};
use zeroize::{Zeroize, Zeroizing};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Algorithm {
    Md5,
    Md5Session,
    Sha256,
    Sha256Session,
    Sha512256,
    Sha512256Session,
}
impl Algorithm {
    fn from_client(client: &DigestClient) -> Result<Self, Error> {
        Ok(match (client.algorithm(), client.session()) {
            (Hash::Md5, false) => Self::Md5,
            (Hash::Md5, true) => Self::Md5Session,
            (Hash::Sha256, false) => Self::Sha256,
            (Hash::Sha256, true) => Self::Sha256Session,
            (Hash::Sha512Trunc256, false) => Self::Sha512256,
            (Hash::Sha512Trunc256, true) => Self::Sha512256Session,
            _ => return Err(Error::AUTHENTICATION),
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Qop {
    None,
    Auth,
    AuthInt,
}
impl Qop {
    fn native(self) -> Option<HttpQop> {
        match self {
            Self::None => None,
            Self::Auth => Some(HttpQop::Auth),
            Self::AuthInt => Some(HttpQop::AuthInt),
        }
    }
}
pub struct Policy {
    pub basic: bool,
    pub algorithms: Vec<Algorithm>,
    pub qops: Vec<Qop>,
    pub realm: Option<String>,
    pub require_server_proof: bool,
}
#[derive(Clone, Debug)]
pub struct Challenge {
    pub id: String,
    pub scheme: &'static str,
    pub realm: String,
    pub algorithm: Option<Algorithm>,
    pub qops: Vec<Qop>,
    pub stale: bool,
}
#[derive(Debug, Default)]
pub struct Observation {
    pub challenges: Vec<Challenge>,
    pub server_proof: Option<bool>,
    pub protected_headers: bool,
}
enum Mechanism {
    Basic(BasicClient),
    Digest(DigestClient),
}
struct Offered {
    summary: Challenge,
    mechanism: Mechanism,
}
pub(crate) struct Prepared {
    pub header: Zeroizing<String>,
    context: String,
    proof: Option<ServerProof>,
}
pub(crate) struct Auth {
    username: Zeroizing<String>,
    password: Zeroizing<String>,
    policy: Policy,
    basic_transport: bool,
    offered: Vec<Offered>,
    pending: Option<Prepared>,
}
impl Auth {
    pub(crate) fn new(
        username: Zeroizing<String>,
        password: Zeroizing<String>,
        policy: Policy,
        basic_transport: bool,
    ) -> Result<Self, Error> {
        if username.len() > 65536
            || password.len() > 65536
            || (!policy.basic && policy.algorithms.is_empty())
            || (!policy.algorithms.is_empty() && policy.qops.is_empty())
        {
            return Err(Error::AUTHENTICATION);
        }
        Ok(Self {
            username,
            password,
            policy,
            basic_transport,
            offered: Vec::new(),
            pending: None,
        })
    }
    pub(crate) fn prepare(
        &mut self,
        id: &str,
        qop: Qop,
        method: &str,
        uri: &str,
        body: &[u8],
    ) -> Result<Prepared, Error> {
        let offered = self
            .offered
            .iter_mut()
            .find(|entry| entry.summary.id == id)
            .ok_or(Error::AUTHENTICATION)?;
        let (header, proof) = match &mut offered.mechanism {
            Mechanism::Basic(client) => {
                if !self.basic_transport || qop != Qop::None {
                    return Err(Error::AUTHENTICATION);
                }
                (
                    client
                        .respond_protected(&self.username, &self.password)
                        .map_err(|_| Error::AUTHENTICATION)?,
                    None,
                )
            }
            Mechanism::Digest(client) => {
                if !offered.summary.qops.contains(&qop) {
                    return Err(Error::AUTHENTICATION);
                }
                let response = client
                    .respond_with_qop_protected(
                        &PasswordParams {
                            username: &self.username,
                            password: &self.password,
                            method,
                            uri,
                            body: Some(body),
                        },
                        qop.native(),
                    )
                    .map_err(|_| Error::AUTHENTICATION)?;
                (response.authorization, Some(response.proof))
            }
        };
        Ok(Prepared {
            header,
            proof,
            context: id.to_owned(),
        })
    }
    pub(crate) fn dispatched(&mut self, mut prepared: Prepared) {
        // The native writer now owns its protected serialization. Retain only
        // the request-specific server proof until its final response arrives.
        prepared.header.zeroize();
        self.pending = Some(prepared);
    }
    pub(crate) fn response(
        &mut self,
        status: i32,
        headers: &mut Vec<(Vec<u8>, Vec<u8>)>,
        body: &[u8],
        raw: &mut Vec<u8>,
    ) -> Result<Observation, Error> {
        let mut observation = Observation::default();
        let mut challenges = Vec::new();
        let mut info = None;
        let mut malformed = false;
        for (name, value) in headers.iter_mut() {
            if name.eq_ignore_ascii_case(b"www-authenticate")
                || name.eq_ignore_ascii_case(b"authentication-info")
                || name.eq_ignore_ascii_case(b"proxy-authenticate")
                || name.eq_ignore_ascii_case(b"proxy-authentication-info")
            {
                observation.protected_headers = true;
                let value = Zeroizing::new(std::mem::take(value));
                if name.eq_ignore_ascii_case(b"www-authenticate") {
                    challenges.push(value);
                } else if name.eq_ignore_ascii_case(b"authentication-info") {
                    if info.replace(value).is_some() {
                        malformed = true;
                    }
                }
            }
        }
        if observation.protected_headers {
            raw.zeroize();
            raw.clear();
        }
        if malformed {
            return Err(Error::AUTHENTICATION);
        }
        if status < 200 {
            return Ok(observation);
        }
        let pending = self.pending.take();
        if status == 401 {
            self.offered.clear();
            for value in challenges {
                let value = std::str::from_utf8(&value).map_err(|_| Error::AUTHENTICATION)?;
                let parsed =
                    http_auth::parse_challenges(value).map_err(|_| Error::AUTHENTICATION)?;
                for challenge in parsed {
                    let mechanism = if challenge.scheme.eq_ignore_ascii_case("Basic") {
                        if !self.policy.basic || !self.basic_transport {
                            continue;
                        }
                        Mechanism::Basic(
                            BasicClient::try_from(&challenge).map_err(|_| Error::AUTHENTICATION)?,
                        )
                    } else if challenge.scheme.eq_ignore_ascii_case("Digest") {
                        let client = DigestClient::try_from(&challenge)
                            .map_err(|_| Error::AUTHENTICATION)?;
                        if !self
                            .policy
                            .algorithms
                            .contains(&Algorithm::from_client(&client)?)
                        {
                            continue;
                        }
                        Mechanism::Digest(client)
                    } else {
                        continue;
                    };
                    let (scheme, realm, algorithm, qops, stale) = match &mechanism {
                        Mechanism::Basic(client) => {
                            ("basic", client.realm(), None, vec![Qop::None], false)
                        }
                        Mechanism::Digest(client) => {
                            let offered = if client.rfc2069_compat() {
                                vec![Qop::None]
                            } else {
                                [Qop::Auth, Qop::AuthInt]
                                    .into_iter()
                                    .filter(|qop| client.qop() & qop.native().unwrap())
                                    .collect()
                            };
                            let qops = offered
                                .into_iter()
                                .filter(|qop| self.policy.qops.contains(qop))
                                .collect::<Vec<_>>();
                            if qops.is_empty() {
                                continue;
                            }
                            (
                                "digest",
                                client.realm(),
                                Some(Algorithm::from_client(client)?),
                                qops,
                                client.stale(),
                            )
                        }
                    };
                    if self
                        .policy
                        .realm
                        .as_deref()
                        .is_some_and(|allowed| allowed != realm)
                    {
                        continue;
                    }
                    let summary = Challenge {
                        id: uuid::Uuid::new_v4().to_string(),
                        scheme,
                        realm: realm.to_owned(),
                        algorithm,
                        qops,
                        stale,
                    };
                    observation.challenges.push(summary.clone());
                    self.offered.push(Offered { summary, mechanism });
                }
            }
            return Ok(observation);
        }
        if let Some(proof) = pending.as_ref().and_then(|pending| pending.proof.as_ref()) {
            match info {
                None if self.policy.require_server_proof => return Err(Error::AUTHENTICATION),
                None => observation.server_proof = Some(false),
                Some(info) => {
                    let next = proof
                        .verify(
                            std::str::from_utf8(&info).map_err(|_| Error::AUTHENTICATION)?,
                            body,
                        )
                        .map_err(|_| Error::AUTHENTICATION)?;
                    observation.server_proof = Some(true);
                    if let Some(next) = next {
                        let next = Zeroizing::new(next);
                        let context = &pending.as_ref().unwrap().context;
                        let offered = self
                            .offered
                            .iter_mut()
                            .find(|offered| &offered.summary.id == context)
                            .ok_or(Error::AUTHENTICATION)?;
                        let Mechanism::Digest(client) = &mut offered.mechanism else {
                            return Err(Error::AUTHENTICATION);
                        };
                        client
                            .adopt_verified_nonce(&next)
                            .map_err(|_| Error::AUTHENTICATION)?;
                        // Explicit subsequent requests select the newly observed handle.
                        offered.summary.id = uuid::Uuid::new_v4().to_string();
                        offered.summary.stale = false;
                        observation.challenges.push(offered.summary.clone());
                    }
                }
            }
        } else if info.is_some() {
            return Err(Error::AUTHENTICATION);
        }
        Ok(observation)
    }
}
