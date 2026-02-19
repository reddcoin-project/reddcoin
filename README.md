ReddCoin Core integration/staging tree
=====================================

https://github.com/reddcoin-project/reddcoin

For an immediately usable, binary version of the ReddCoin Core software, see
https://download.reddcoin.com or https://github.com/reddcoin-project/reddcoin

Further information about ReddCoin Core is available in the [doc folder](/doc).

What is ReddCoin?

ReddCoin (RDD) is a decentralized blockchain network launched in 2014 and designed for social payments, tipping, and micro-transactions.

The network operates using a Proof-of-Stake-Velocity (PoSV v2) consensus mechanism and enables low-cost peer-to-peer digital value transfer without centralized control.

ReddCoin Core is the open-source software implementation that maintains the network and allows users to participate in transaction validation and staking.

For more information, refer to the ReddCoin ReddPaper and documentation available at https://reddcoin.com and https://redd.love.

License
-------

ReddCoin Core is released under the terms of the MIT License. See COPYING for more information or visit https://opensource.org/licenses/MIT.

Development Process
-------------------

The `master` branch is regularly built (see `doc/build-*.md` for instructions) and tested, but it is not guaranteed to be completely stable.

Tags are created from release branches to indicate new official, stable release versions of ReddCoin Core.

The contribution workflow is described in [CONTRIBUTING.md](CONTRIBUTING.md), and additional guidance for developers can be found in [doc/developer-notes.md](doc/developer-notes.md).

Testing
-------

Testing and code review are critical components of the development process. The number of pull requests may at times exceed immediate review capacity. Contributors are encouraged to assist by testing open pull requests where possible.

ReddCoin Core is a security-critical project. Careful review and thorough testing help ensure network stability and reliability.

### Automated Testing

Developers are strongly encouraged to write [unit tests](src/test/README.md) for new code and to improve coverage for existing code where appropriate.

Unit tests can be compiled and run (assuming they were not disabled during configuration) with:
`make check`

Additional details on running and extending unit tests are available in [/src/test/README.md](/src/test/README.md).

Regression and integration tests are located in [/test](/test) and written in Python. These tests can be executed (if the required [test dependencies](/test) are installed) with:
`test/functional/test_runner.py`

Continuous Integration (CI) systems ensure that pull requests are built on Windows, Linux, and macOS, and that automated tests are executed.

### Manual Quality Assurance (QA) Testing

Changes should be tested by a contributor other than the original author, particularly for large or higher-risk modifications.

Where appropriate, a clear test plan should be included in the pull request description to facilitate independent verification.

Translations
------------

Changes to existing translations and new translations should be submitted via [ReddCoin Core's Transifex page](https://www.transifex.com/reddcoin/reddcoin/).

Translations are periodically synchronized from Transifex and merged into the repository. See the [translation process](doc/translation_process.md) for details.

**Important:** Translation changes are not accepted via GitHub pull requests, as they would be overwritten during the next synchronization from Transifex.
