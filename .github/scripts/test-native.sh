#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:-build-ludeo}"
meson setup "$build_dir" --buildtype=debugoptimized --wrap-mode=nofallback -Dauto_features=disabled -Dbase=enabled -Dgood=enabled -Dbad=enabled -Dtests=enabled -Dgstreamer:check=enabled -Dtools=enabled -Dwebrtc=enabled -Dgst-plugins-base:app=enabled -Dgst-plugins-good:rtp=enabled -Dgst-plugins-good:rtpmanager=enabled -Dgst-plugins-bad:videoparsers=enabled -Dgst-plugins-bad:nvcodec=enabled -Dgst-plugins-bad:webrtc=enabled -Dgst-plugins-bad:dtls=enabled -Dgst-plugins-bad:srtp=enabled -Dgst-plugins-bad:sctp=enabled -Dgst-plugins-bad:sctp-internal-usrsctp=disabled
meson compile -C "$build_dir" -j "${BUILD_JOBS:-4}"
meson test -C "$build_dir" --no-rebuild --print-errorlogs --num-processes 2 elements_rtprtx elements_rtpred elements_rtpulpfec elements_rtpbin elements_rtpfunnel elements_rtph264 elements_h264parse libs_h264parser
