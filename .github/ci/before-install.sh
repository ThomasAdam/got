#!/bin/sh

if [ "$CIRRUS_OS" = "linux" ]; then
	apt-get update -qq && \
	apt-get -y install bison \
				autotools-dev \
				libncurses5-dev \
				pkg-config \
				build-essential \
				libmd-dev \
				libssl-dev \
				libbsd-dev \
				uuid-dev \
				zlib1g-dev
fi

if [ "$CIRRUS_OS" = "freebsd" ]; then
	pkg install -y \
		automake \
		pkgconf
fi

if [ "$CIRRUS_OS" = "darwin" ]; then
	brew install autoconf \
		automake \
		pkg-config \
		ncurses \
		ossp-uuid
fi
