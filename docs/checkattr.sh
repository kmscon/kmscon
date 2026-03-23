#!/bin/bash

underline=`tput smul`
nounderline=`tput rmul`
bold=`tput bold`
normal=`tput sgr0`
invert=`tput smso`
noinvert=`tput rmso`

echo "${bold}bold${normal} text stands out!"
echo "${underline}underlined${nounderline} text does, too."

echo "▛▜"
echo "▙${invert}▘${noinvert}"


