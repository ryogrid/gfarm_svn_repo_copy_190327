#!/bin/sh

# defines
CONF_FILE=/etc/zabbix/externalscripts/zbx_chk_gfarm.conf

# check config file
if [ -f $CONF_FILE ];
    then
    . $CONF_FILE
else
    echo  -1;
    exit 0;
fi

# exec check command
if [ X"$MY_HOST" != X ]; then
    FSN_NAME=$MY_HOST
else
    FSN_NAME=`hostname`
fi

RESULT=`gfhost $* | grep $FSN_NAME`

if [ $? != 0 ];
then
    RESULT="gfhost $* error."
fi

echo $RESULT

exit 0
