#!/bin/bash

set -e
set -x

PYVER=`python -c 'import sys; print(".".join(map(str, sys.version_info[:2])))'`

python -V

# setup OSX
if [[ "$(uname -s)" == 'Darwin' ]]; then
    if which pyenv > /dev/null; then
        eval "$(pyenv init -)"
    fi
    pyenv activate psutil
fi

# install psutil
python setup.py build
python setup.py develop

# run tests
if [[ $PYVER == '2.6' ]]; then
    python -c "import psutil"
else if [[ $PYVER == '2.7' ]] && [[ "$(uname -s)" != 'Darwin' ]]; then
    coverage run psutil/tests/__main__.py
else
    python psutil/tests/__main__.py
fi

if [ "$PYVER" == "2.7" ] || [ "$PYVER" == "3.6" ]; then
    # run mem leaks test
    python psutil/tests/test_memory_leaks.py
    # run linter (on Linux only)
    if [[ "$(uname -s)" != 'Darwin' ]]; then
        python -m flake8
    fi
fi
