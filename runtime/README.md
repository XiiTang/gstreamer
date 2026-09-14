# IMAPipe native media integration

This fork keeps GStreamer's media engines and native state behind IMAPipe's
execution-owned network and resource boundary. The Runtime adapter and packaging
are separate acceptance gates; the tests here are native-library evidence.

## DTLS-SRTP

- `gst_dtls_connection_set_srtp_profiles` selects only the declared profiles,
  before the first handshake. Each connection permits one handshake. A new
  connection is required for a new security session.
- Supported profiles: AES128_CM_SHA1_80 (30 bytes), AEAD_AES_128_GCM (28 bytes),
  AEAD_AES_256_GCM (44 bytes). Exporter layout is client key, server key, client
  salt, server salt. Native encoder/decoder key signals include the actual length.
- A positive peer certificate decision is required before exporting keys.
  `dtlsdec::accept-peer-certificate` defaults to rejection. The Runtime must
  install the verification decision from its frozen identity policy; observing
  `peer-pem` is insufficient.
- Temporary exporter material and retained key buffers are cleansed. Native
  decoder/encoder key logging is removed. General object/caps diagnostics still
  require the production adapter to keep key-bearing objects private.
- Timer registrations hold weak references and are replaced/cancelled explicitly.
  Stopping or releasing an incomplete handshake does not retain the connection
  until the next timer expiration and sends no close-notify.

`tests/build_dtls.py` takes explicit native dependency/configuration paths and
builds both the complete DTLS plugin and `tests/dtls_interop.c`. The latter uses
an independent OpenSSL DTLS server over isolated loopback UDP. It checks exact
exporter bytes, fresh key material, trust rejection, missing verification,
non-overlapping profile offers, restart rejection and 64 pending-timer lifetimes.
No test key is logged or saved to disk. This build helper is for native tests;
it is not the product's dependency discovery or packaging policy.
