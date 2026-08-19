#!/bin/bash
set -e

echo "Building KEYSTONE for native architecture via Makefile..."
make clean
make all
echo "Native build completed successfully!"
