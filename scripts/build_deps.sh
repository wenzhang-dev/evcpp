#!/bin/bash

function build_libev() {
    pushd third-party/libev

    rm -rf build
    echo "build dir: $(pwd)/build"
    mkdir -p build
    
    ./configure --prefix=$(pwd)/build
    make -j8
    make install

    popd
}

build_libev
