#!/bin/bash
set -euxo pipefail

dockerimage="debian:11-slim"


# we do this so that we can be agnostic about where we're invoked from
# meaning you can exec this script anywhere and it should work the same
thisiswhereiam=${BASH_SOURCE[0]}
script_folder=$( cd -- "$( dirname -- "${thisiswhereiam}" )" &> /dev/null && pwd )


# this should be /whatever/directory/structure/[accelerator_root]/dockerbuild
build_dir="dockerbuild"

if command -v podman >/dev/null 2>&1; then
    container_runtime="podman"
elif command -v docker >/dev/null 2>&1; then
    container_runtime="docker"
else
    echo "Neither podman nor docker is available." >&2
    exit 127
fi

pushd "${script_folder}" &> /dev/null || exit 99

    internalscript="_accelerator_docker_build_internal.sh"

    pushd ../ &> /dev/null
        dev_srcdir=$(pwd)
        container_rootdir="accelerator"

        itflag=""
        if [ -t 0 ] ; then
            itflag="-it"
        fi

        "${container_runtime}" run ${itflag}                 \
        -v "${dev_srcdir}":/"${container_rootdir}"           \
        -w /${container_rootdir}                             \
        ${dockerimage}                                       \
        bash ./${build_dir}/${internalscript} "$@"

        ecodereal=$?
        echo "real exit code ${ecodereal}"

    popd &> /dev/null || exit

popd &> /dev/null || exit

exit ${ecodereal}
