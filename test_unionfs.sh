#!/bin/bash

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./test_env"
LOWER="$TEST_DIR/lower"
UPPER="$TEST_DIR/upper"
MNT="$TEST_DIR/mnt"

echo "Setting up..."
rm -rf $TEST_DIR
mkdir -p $LOWER $UPPER $MNT

echo "base_only" > $LOWER/base.txt
echo "delete_me" > $LOWER/delete_me.txt

# Run FS in background
$FUSE_BINARY $LOWER $UPPER $MNT &
PID=$!
sleep 2

echo "Test 1: Read"
cat $MNT/base.txt

echo "Test 2: Copy-on-Write"
echo "modified" >> $MNT/base.txt
cat $UPPER/base.txt
cat $LOWER/base.txt

echo "Test 3: Whiteout"
rm $MNT/delete_me.txt
ls -la $UPPER

echo "Test 4: mkdir"
mkdir $MNT/newdir
ls $UPPER

echo "Test 5: rmdir"
rmdir $MNT/newdir
ls $UPPER

# Cleanup
kill $PID
fusermount -u $MNT 2>/dev/null
rm -rf $TEST_DIR

echo "Done."
