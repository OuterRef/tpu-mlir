#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

export PROJECT_ROOT=$DIR
export BUILD_PATH=${BUILD_PATH:-$PROJECT_ROOT/build}
export INSTALL_PATH=${INSTALL_PATH:-$PROJECT_ROOT/install}
export TPUC_ROOT=$INSTALL_PATH

echo "PROJECT_ROOT : ${PROJECT_ROOT}"
echo "BUILD_PATH   : ${BUILD_PATH}"
echo "INSTALL_PATH : ${INSTALL_PATH}"

# regression path
export REGRESSION_PATH=$PROJECT_ROOT/regression
export NNMODELS_PATH=${PROJECT_ROOT}/../nnmodels
export MODEL_ZOO_PATH=${PROJECT_ROOT}/../model-zoo

# run path
export PATH=$INSTALL_PATH/bin:$PATH
export PATH=$PROJECT_ROOT/python/tools:$PATH
export PATH=$PROJECT_ROOT/python/utils:$PATH
export PATH=$PROJECT_ROOT/python/test:$PATH
export PATH=$PROJECT_ROOT/python/samples:$PATH
export PATH=$PROJECT_ROOT/third_party/customlayer/python:$PATH

export CMODEL_LD_LIBRARY_PATH=$INSTALL_PATH/lib:$PROJECT_ROOT/capi/lib:$LD_LIBRARY_PATH
export CHIP_LD_LIBRARY_PATH=/opt/sophon/libsophon-current/lib/:$INSTALL_PATH/lib:$PROJECT_ROOT/capi/lib:$LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$CMODEL_LD_LIBRARY_PATH
export USING_CMODEL=True
export PYTHONPATH=$INSTALL_PATH/python:$PYTHONPATH
export PYTHONPATH=/usr/local/python_packages/:$PYTHONPATH
export PYTHONPATH=$PROJECT_ROOT/python:$PYTHONPATH
export PYTHONPATH=$PROJECT_ROOT/third_party/customlayer/python:$PYTHONPATH

export OMP_NUM_THREADS=4

# CCache configuration
export CCACHE_REMOTE_STORAGE=redis://10.132.3.118:6379

function use_cmodel(){
    export USING_CMODEL=True
    export LD_LIBRARY_PATH=$CMODEL_LD_LIBRARY_PATH
}
function use_chip(){
    export USING_CMODEL=False
    export LD_LIBRARY_PATH=$CHIP_LD_LIBRARY_PATH
}
