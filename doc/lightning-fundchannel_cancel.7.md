lightning-fundchannel\_cancel -- Command for completing channel establishment
=============================================================================

SYNOPSIS
--------

**fundchannel\_cancel** *id*

DESCRIPTION
-----------

`fundchannel_cancel` is a lower level RPC command. It allows a user to
cancel an initiated channel establishment with a connected peer.

*id* is the node id of the remote peer with which to cancel the

RETURN VALUE
------------

On success, returns confirmation that the channel establishment has been
canceled.

On failure, returns an error.

AUTHOR
------

Lisa Neigut <<niftynei@gmail.com>> is mainly responsible.

SEE ALSO
--------

lightning-connect(7), lightning-fundchannel(7),
lightning-fundchannel\_start(7), lightning-fundchannel\_complete(7)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>
