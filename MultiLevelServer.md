# Introduction #

Our Netopeer multi-level NETCONF server follows architecture described [here](http://libnetconf.googlecode.com/git/doc/doxygen/html/da/db3/server.html). The source codes of this application can be found in the project Git repository inside the `/server/` directory. From here, this server is referred as the _Netopeer server_.

The Netopeer server is the main part of the Netopeer project.
It uses [libnetconf](https://code.google.com/p/libnetconf/) for handling NETCONF messages and applying operations. Running in background as a system daemon, it allows centralized access to managed devices.
It allows multiple access from clients through agents connected through the [D-Bus](http://www.freedesktop.org/wiki/Software/dbus/) system message bus or UNIX sockets (when compiled with _--disable-dbus_).

Netopeer server utilize libnetconf [transAPI](http://libnetconf.googlecode.com/git/doc/doxygen/html/d9/d25/transapi.html) modules to control devices (from here, referred as the _Netopeer modules_). The default module, called Netopeer, is responsible for managing other Netopeer modules specified in the Netopeer configuration data (more described below).

# Requirements #

Before compiling the source code, make sure that your system provides the following libraries or applications. Some of them are optional or can be avoided in cost of missing of some feature - see the notes for the specific item. All requirements are checked by the `configure` script.

  * compiler (_gcc_, _clang_,...) and standard headers
  * _pkg-config_
  * _libpthreads_
  * _libxml2_ (including headers from the devel package)
  * _libnetconf_ (including headers from the devel package)
  * _libdbus_ (including headers from the devel package)
    * can be omitted by `--disable-dbus` configure's option. In that case standard UNIX sockets are used for communication between **netopeer-server(8)** and **netopeer-agent(1)**s.
  * python 2.6 or higher with the following modules:
    * os, string, re, argparse, subprocess, curses, xml, libxml2
  * _sshd_
    * openSSH's SSH server, make sure that the correct path to this binary is part of your `PATH` environment variable or use `--with-sshd=/path/to/sshd` configure's option.
  * stunnel
    * required only when the TLS transport is enabled by `--enable-tls` option. Make sure that the correct path to this binary is part of your `PATH` environment variable or use `--with-stunnel=/path/to/stunnel` configure's option.
  * roff2html
    * optional, used for building HTML version of man pages (make doc)
  * rpmbuild
    * optional, used for building RPM package (make rpm).

# Installation #

Notorious sequence
```
./configure && make && make install
```
will configure, build project and install the following binaries:
  1. [netopeer-server(8)](http://netopeer.googlecode.com/git/server/netopeer-server.8.html) - the Netopeer server
  1. [netopeer-agent(1)](http://netopeer.googlecode.com/git/server/netopeer-agent.1.html) - NETCONF agent running as an SSH subsystem and comunicating with the Netopeer server over D-Bus or UNIX socket.
  1. [netopeer-manager(1)](http://netopeer.googlecode.com/git/server/manager/netopeer-manager.1.html) - tool used to manage the Netopeer modules
  1. [netopeer-configurator(1)](http://netopeer.googlecode.com/git/server/configurator/netopeer-configurator.1.html) - tool used for the Netopeer server first run configuration (mainly focus on NACM section)

  * **Note**: Do not change SSH daemon settings. The Netopeer server uses its own instance of the SSH daemon which is configured via NETCONF according to the ietf-netconf-server data model. By default, the server will listen on port 830, so it is good to make sure that no other application (including the default SSH daemon) does not use this port.

# Usage #
## Managing the Netopeer modules ##
The Netopeer server needs a configuration XML file for each module with path to the .so file, paths to data models and the datastore. The data models are loaded in their order in the configuration to account for any import/augment situation. To manage the Netopeer modules, there is the **netopeer-manager(1)** tool. For more information about using this tool, see the [man page](http://netopeer.googlecode.com/git/server/manager/netopeer-manager.1.html):
```
$ man netopeer-manager
```

## First run configuration ##
Before the starting **netopeer-server(8)**, we recommend to configure it using the **netopeer-configurator(1)** tool. It takes information from the compilation process and allows you to change (or at least to show) various **netopeer-server(8)** settings.

The following subsections describes what you can change using **netopeer-configurator(1)**.

### Netopeer ###
This section shows where the Netopeer binaries were installed. Furthermore, it allows user to disable/enable the Netopeer modules added by **netopeer-manager(1)**

### NACM ###
This section covers NETCONF Access Control Module configuration. By default NACM avoids any write to the configuration data. To change this, the user can here the default write action or specify the user(s) with unlimited access. There are more configuration switches including a possibility to completely turn off the NACM.

### Intercommunication ###
This tab provides information about used communication technology between the Netopeer server and agents. Currently, there are two possibilities: D-Bus and UNIX sockets. In case of D-Bus, user can specify here permissions what users group is allowed to connect to the Netopeer server. In case of UNIX sockets, this setting can be changed only by recompiling the server with different value of the _--with-group_ option. When the Netopeer server is compiled without this option, access to the Netopeer server is not limited. However, there is NACM which can be used to fine-grained access control.

## Starting the server ##
If the server was compiled with _--with-dbus-services_ option, the **netopeer-server** is started automatically by D-Bus with the first incoming connection. Otherwise, it must be started manually.
```
# netopeer-server -d
```

The _-d_ option makes the server to start in a daemon mode. You can also set logging verbosity by specifying parameter for the _-v_ option from 0 to 3 (errors, warnings, verbose, debug).

When the Netopeer server starts, it automatically initiate the Netopeer build-in module. The module loads its startup configuration and manages all other modules added by the **netopeer-manager(1)** tool. When the module is added, it is enabled by default. To disable starting a specific module, **netopeer-configurator(1)** can be used or it can be done directly via NETCONF modifying the Netopeer's configuration data.

**Example**: disable _my\_magic\_module_:
```
<rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
  <edit-config>
    <target>
      <running/>
    </target>
    <config xmlns:xc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <netopeer xmlns="urn:cesnet:tmc:netopeer:1.0">
        <modules>
          <module>
            <module-name>my-magic-module</module-name>
            <module-allowed xc:operation="merge">false</module-allowed>
          </module>
        </modules>
      </netopeer>
    </config>
  </edit-config>
</rpc>
```