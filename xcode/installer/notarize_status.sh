#!/bin/sh
#
# Run this script to check the status of outstanding notarization requests.
# The <request_uuid> can be found in notarization.log or by running:
#
#   xcrun altool --notarization-history 0 -u <appe_id> \
#       -p "@keychain:notarize-apple-pass"
#

if [ -z "$1" -o -z "$2" ]; then
  echo "Usage: notarize_status.sh <apple_id> <request_uuid>"
  exit 1
fi

xcrun altool --notarization-info "$2" \
  --username "$1" \
  --password "@keychain:notarize-apple-pass"
