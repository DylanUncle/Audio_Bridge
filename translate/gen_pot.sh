#!/bin/sh

FILES="AudioBridge.cpp"

COPYRIGHT_HOLDER="Dylan"
PKG_NAME="AudioBridge"

cd ..
xgettext -o translate/source/messages.pot --c++ --add-comments=/ --keyword=_ --keyword=C_:1c,2 --copyright-holder="$COPYRIGHT_HOLDER" --package-name="$PKG_NAME" $FILES
