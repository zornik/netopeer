This text describes user access control in case of **[netopeer-server(8)](MultiLevelServer.md)**

# Problem Statement #

In some cases, it can be useful to limit group of users allowed to access the Netopeer server. Access to a specific configuration data or NETCONF operation can be in detail specified using NACM.

# Generic User Access #

This section describes how to specify a group of users allowed to communicate with the **netopeer-server(8)**. This can be done by limiting users allowed to use intercommunication channel between **netopeer-agent(1)** and **netopeer-server(8)**. There are two possibilities of intercommunication: D-Bus and Unix sockets.

By default, all users are allowed to use intercommunication channel. If you prefer to limit this access, use _--with-group_ option of the _configure_ script.

## D-Bus ##

D-Bus is a default intercommunication channel. In this case, access control is specified in the D-Bus configuration file for Netopeer service _org.liberouter.netopeer.conf_ (usually `/etc/dbus-1/system.d/org.liberouter.netopeer.conf`). Netopeer allows user to _a)_ allow access to anybody or _b)_ specify a single group with granted access. The group can be specified via configure's _--with-group_ option or via _Intercommunication_ section in **netopeer-configurator(1)**. However, D-Bus configuration allows you to specify more groups or just specific users with access to the service. For more information, see the D-Bus [configuration file description](http://dbus.freedesktop.org/doc/dbus-daemon.1.html#configuration_file).

## UNIX socket ##

UNIX socket is an alternate communication channel when D-Bus cannot be used. To switch to the UNIX socket communication, use configure's _--disable-dbus_ option. In this case, user access to the communication channel is done via access rights to the socket file. Therefore, is it possible to allow access to all users (by default) or limit it to a single group (with the configure's _--with-group_ option). Furthermore, this setting is compiled into the **netopeer-server(8)** and it can be changed only by recompiling Netopeer server with different value to the _--with-group_ option.


# NACM #

NACM refers to _NETCONF Access Control Module_ and it is described in [RFC 6536](http://tools.ietf.org/html/rfc6536) in details. According to this document, NACM is initially set to allow reading (permitted read-default), refuse writing (denied write-default) and allow operation execution (permitted exec-default). The disabled writing can make some problems in the beginning of the Netopeer usage.

## Recovery Session ##

If a session is recognized as recovery, NACM subsystem is completely bypassed. It serves for setting up initial access rules or to repair a broken access control configuration.

Recovery session is identified according to the UID of connected user. Decision if the session is recovery or not is done by [libnetconf](https://code.google.com/p/libnetconf/). By default, all sessions of the user with the system UID equal zero (root) are considered as recovery. To change this default value to a UID of any user, run libnetconf's configure with _--with-nacm-recovery-uid_ option and recompile the libnetconf library.

## Initial Operation ##

To change the initial NACM settings denying data modification, user has to access NACM datastore via a recovery session and set required access control rules.

For example, to change default write rule from deny to permit, use `edit-config` operation to create (merge) the following configuration data:

```
<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
  <write-default>permit</write-default>
</nacm>
```

To guarantee all access rights to a specific users group, use `edit-config` operation to create (merge) the following rule:

```
<nacm xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
  <rule-list>
    <name>admin-acl</name>
    <group>admin</group>
    <rule>
      <name>permit-all</name>
      <module-name>*</module-name>
      <access-operations>*</access-operations>
      <action>permit</action>
    </rule>
  </rule-list>
</nacm>
```

Alternatively, you can use the NACM section of the **netopeer-configurator(1)** to change default action or to add user(s) with unlimited access to the configuration datastores.