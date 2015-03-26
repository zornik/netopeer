The **cfgsystem** transAPI module is located inside the `transAPI/cfgsystem/` directory in the Netopeer GIT repository.

  * **Note**: This module accesses and modifies system configuration files. Use it on your own risk. It can harm your system configuration files.

# cfgsystem.so #

TransAPI module primarily intended for the Netopeer server, but in general for libnetconf based NETCONF servers. The module should correctly work on RHEL, SUSE and Debian based distros.

## Installation ##

```
$ ./configure
$ make
# make install
```

### configure options ###

`--with-netopeer-confdir=DIR`

> If you changed the sysconfdir of the Netopeer server, please reflect this change in this option. By default, the Netopeer server stores its configuration files in ${sysconfdir}/netopeer/ directory. Instead of the complete path, you can alternatively specify the sysconfdir value (`--sysconfdir=DIR`) in the same way as in case of the Netopeer server.

`--with-[useradd|userdel|shutdown]=PATH`

> On some distros (RHEL based) when a normal user builds (run configure) it is unable to detect, that these tools are available when you are root. Therefore, if configure cannot detect those tools, you can specify them manually. Otherwise, configure switches off the functionality of the cfgsystem that depends on that tools.

`--enable-tls`

> This enables all the TLS features of cfgsystem such as trusted client
and CA certificates or cert-to-name feature.

## Functionality ##

This module implements the **ietf-system** data model as defined in <a href='http://tools.ietf.org/html/rfc7317'>RFC 7317</a>. It implements only the folowing features of the data model:

  * **authentication** - configuration of the user authentication by manipulating with the sshd\_config of SSH daemon listening for the NETCONF connections.
  * **local-users** - configuration of local user authentication by manipulating with the `/etc/passwd`, `/etc/shadow` and `~/.ssh/authorized_keys` files.
  * **ntp** - configuration of usage NTP server(s) by manipulating with `/etc/ntp.conf` file and the ntp (or ntpd on RHEL based distros) service.
  * **timezone-name** - allows timezone configuration using TZ database.

The module also allows configuration of:

  * **DNS resolver** by manipulating with the `/etc/resolv.conf`.

For the detailed description of content for the specific configuration element, please read the <a href='http://tools.ietf.org/html/rfc7317'>RFC 7317</a>.

### TLS ###

Augment model ietf-system-tls-auth from May 24, 2014 is added to
the ietf-system model with features:

  * **tls-map-certificates** - cert-to-name information for the server.

cert-to-name configuration will after installation include one entry
resolving `server/stunnel/client.pem` certificate into the _tls\_default_
username.

### tls-map-certificates feature ###

This feature does not fully comply with _ietf-system-tls-auth_ description.
Only peer certificates (not every certificate encountered while traversing
the CA chain during verification, as is required) are checked for a matching
cert-to-name entry in the configuration. For this reason it does not make
much sense to use any other map-type than `specified` even though all
map-types are implemented and work correctly with the aforementioned
restriction.

### environment variables ###

cfgsystem relies on retrieving some information from the environment
variables exported by **netopeer-server(8)**, namely:

  * **SSHD\_PID** - PID number of the sshd process
  * **STUNNEL\_PID** - PID number of the stunnel process
  * **STUNNEL\_CA\_PATH** - path to stunnel trusted CA directory
  * **C\_REHASH\_PATH** - path to the c\_rehash utility

# nc\_cfgsystem.py #

This is a module for **netopeer-configurator(1)** to enable user-friendly
manipulation of `PasswordAuthentication` parameter of netopeer sshd\_config file. Installing cfgsystem plugin automatically disables PAM authentication and if `PasswordAuthentication` is disabled as well, the only means of connecting to **netopeer-server(8)** using SSH left are SSH keys. If enabling local user authentication in cfgsystem configuration and having `PasswordAuthentication` set to "`yes`", then it is also possible to connect using a local user identity.

# cfgsystem-init #

This small utility is able to load all the configurations managed by the ietf-system model and store them as the startup configuration data for use in the **netopeer-server(8)**.

The tool is automatically used with `make install` when the Netopeer server (**netopeer-manager(1)**) is already installed, so after that, you don't need to run it manually.

Remember, that having empty startup datastore on **netopeer-server(8)** startup with the cfgsystem module enabled causes removing all current configuration settings (NTP and DNS servers, users,...)!

## Usage ##

```
# ./cfgsystem-init <cfgsystem's datastore path>
```