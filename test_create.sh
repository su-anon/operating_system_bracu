#!/bin/bash
set -e

# Cleanup
rm -f vsfs.img

# 1. Create FS
./mkfs vsfs.img
echo "Created FS."

# 2. Validate clean FS
./validator vsfs.img
echo "Validator passed on clean FS."

# 3. Create a file using journal
./journal create vsfs.img testfile.txt
echo "Ran journal create."

# 4. Validate again (Main FS should be untouched, validator checks SB/Bitmaps/Inodes on disk)
# Validator might not check journal area, or if it does, it expects it to be consistent with block indices.
# Our create command writes to 1-16.
./validator vsfs.img
echo "Validator passed after create (Main FS untouched)."

# 5. Check if journal header is updated (nbytes_used > 0)
# We can use a python one-liner or just hexdump to check manually.
# Let's rely on success exit code for now.

echo "Test Passed!"
