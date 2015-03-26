# Introduction #

NETCONF over TLS is almost fully implemented in netopeer according to the relevant RFC. However, many of these RFCs were still drafts or had some ambiguous parts and wordings at the time of the implementation, so it likely does not completely conform to the RFC specification. Any known deviations are mentioned in the last part of this page.

# General NETCONF TLS transport #

There are always 2 sides of a connection, the server and the client. Once an unsecure connection is established, these two entities exchange their certificates and both verify the other certificate. Based on the RFCs available at the time of the implementation (liable to change), either a standard trusted CA chain verification takes place before accepting the peer certificate or the particular presented peer certificate is already considered trustworthy and CA chain verification is not necessary. Common TLS verification is finished at this point, but in NETCONF there a few additional steps.

## server ##

Having successfully verified the client certificate, the server must somehow obtain a NETCONF username of the client to restrict their permissions accordingly. Since all the server is offered is a certificate, it must derive the username from the certificate and this process is called _cert-to-name_.

Specifics of this process can be learnt from _ietf-system-tls-auth_ model and the corresponding RFC. Our implementation skips quite a large portion of this process, for more information refer to the last section on this page.

## client ##

Beside the aforementioned certificate verification, the client should also perform a check of the actual hostname it used to connect to a server and the one (or one from several) extracted from the server certificate. Our client does not perform this check, please refer to the last section.

# netopeer-server(8) #

After installation, **netopeer-server(8)** is configured to use example certificates for both peer verification and to present itself with. This common part of TLS verification is actually handled by **stunnel(8)**, which then decrypts or encrypts and tunnels the data between the client and the server. Nevertheless, the server certificate, its trusted Certificate Authority store and Certificate Revocation Lists can be fully customized using **netopeer-configurator(1)**.

Certificate Revocation Lists (CRLs) are special certificates that include a list of signed certificates by a certain CA that were for some reason revoked and are no longer considered trustworthy. Basic support for this is implemented and if correctly configured, the CRLs will be checked for client certificate revocation. However, you must manually download CRLs of your trusted Certificate Authorities and keep this list up-to-date.

cert-to-name (CTN) configuration is part of _ietf-system-tls-auth_ augment model for _ietf-system_ model. **cfgsystem** netopeer plugin is our implementation of this model and is required for CTN and therefore TLS over NETCONF to work. Its configuration will include one entry by default resolving the example client certificate to the username **tls\_default**.

# netopeer-cli(1) #

Having completed the installation, **netopeer-cli(1)** will not include any certificates. For the simplest working configuration use CLI command _cert_ to arrange for **netopeer-cli(1)** to use the example client certificate and import its example CA signer certificate, which are supplied with it.

CRL support is included in the client as well, the complete management of these certificates is accomplished by the _crl_ command.

Also, it is possible to use specific client certificate and/or trusted CA store for a single connection (ignoring the defaults) if these are supplied as arguments to the corresponding _connect_ or _listen_ commands.

# Unimplemented/skipped verification steps #

There are 2 steps that are not performed during TLS verification:

**netopeer-server(8) - CA chain certificate cert-to-name feature**

Once the peer certificate is presented, the verification process is started by finding a trust anchor - a certificate that we trust and is part of the peer certificate signature chain. In addition to the standard TLS verification, this certificate (and every one down the chain including the peer certificate itself) should be checked for a match in the cert-to-name configuration and a NETCONF username should be derived if possible. In our implementation the cert-to-name lookup is skipped except the peer certificate. This means that unless you have an explicit entry in the cert-to-name configuration for the certificate presented by the peer, they will not connect to **netopeer-server(8)**, because it will not be able to provide them with a NETCONF username.

**netopeer-cli(1) - Hostname check against the names in the certificate**

To accept server certificate and consider it valid, every NETCONF client should compare the hostname it used to connect to the server with the names (_commonName_ and/or _subjectAltNames_) presented in the certificate. This check is skipped in **netopeer-cli(1)**.