lightning-txprepare -- Command to prepare to withdraw funds from the internal wallet
====================================================================================

SYNOPSIS
--------

**txprepare** *outputs* \[*feerate*\] \[*minconf*\]

DESCRIPTION
-----------

The **txprepare** RPC command creates an unsigned transaction which
spends funds from c-lightning’s internal wallet to the outputs specified
in *outputs*.

The *outputs* is the array of output that include *destination*
and *amount*(\{*destination*: *amount*\}). Its format is like:
\[\{address1: amount1\}, \{address2: amount2\}\]
or
\[\{address: *all*\}\].
It supports the any number of outputs.

The *destination* of output is the address which can be of any Bitcoin accepted
type, including bech32.

The *amount* of output is the amount to be sent from the internal wallet
(expressed, as name suggests, in amount). The string *all* can be used to specify
all available funds. Otherwise, it is in amount precision; it can be a whole
number, a whole number ending in *sat*, a whole number ending in *000msat*,
or a number with 1 to 8 decimal places ending in *btc*.

**txprepare** is similar to the first part of a **withdraw** command, but
supports multiple outputs and uses *outputs* as parameter. The second part
is provided by **txsend**.

RETURN VALUE
------------

On success, an object with attributes *unsigned\_tx* and *txid* will be
returned. You need to hand *txid* to **txsend** or **txdiscard**, as the
inputs of this transaction are reserved until then, or until the daemon
restarts.

*unsigned\_tx* represents the raw bitcoin transaction (not yet signed)
and *txid* represent the bitcoin transaction id.

On failure, an error is reported and the transaction is not created.

The following error codes may occur:
- -1: Catchall nonspecific error.
- 301: There are not enough funds in the internal wallet (including
fees) to create the transaction.
- 302: The dust limit is not met.

AUTHOR
------

Rusty Russell <<rusty@rustcorp.com.au>> is mainly responsible.

SEE ALSO
--------

lightning-withdraw(7), lightning-txsend(7), lightning-txdiscard(7)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>
