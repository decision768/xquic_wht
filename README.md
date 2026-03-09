# XQUIC
<img src="docs/images/xquic_logo.png" alt="xquic logo" width=585.9 height=309.1/>

![GitHub](https://img.shields.io/github/license/alibaba/xquic)
[![Build](https://github.com/alibaba/xquic/actions/workflows/build.yml/badge.svg)](https://github.com/alibaba/xquic/actions/workflows/build.yml)
[![CodeQL](https://github.com/alibaba/xquic/actions/workflows/codeql-analysis.yml/badge.svg)](https://github.com/alibaba/xquic/actions/workflows/codeql-analysis.yml)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/485e758edd98409bb7a51cbb803838c4)](https://www.codacy.com/gh/alibaba/xquic/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=alibaba/xquic&amp;utm_campaign=Badge_Grade)
[![Codacy Badge](https://app.codacy.com/project/badge/Coverage/485e758edd98409bb7a51cbb803838c4)](https://www.codacy.com/gh/alibaba/xquic/dashboard?utm_source=github.com&utm_medium=referral&utm_content=alibaba/xquic&utm_campaign=Badge_Coverage)
![Platforms](https://img.shields.io/badge/platform-Android%20%7C%20iOS%20%7C%20Linux%20%7C%20macOS-blue)

> [简体中文文档 README-zh-CN](https://github.com/alibaba/xquic/blob/main/docs/docs-zh/README-zh.md)

## Introduction

XQUIC Library released by Alibaba is …

… **a client and server implementation of QUIC and HTTP/3 as specified by the IETF.** Currently supported QUIC versions are v1 and draft-29.

… **OS and platform agnostic.** It currently supports Android, iOS, HarmonyOS, Linux, macOS and Windows(v1.2.0). Most of the code is used in our own products, and has been tested at scale on android, iOS apps, as well as servers.

… **still in active development.** [Interoperability](https://interop.seemann.io/) is regularly tested with other QUIC implementations.

### Features

[![](https://img.shields.io/static/v1?label=RFC&message=9000&color=brightgreen)](https://tools.ietf.org/html/rfc9000)
[![](https://img.shields.io/static/v1?label=RFC&message=9001&color=brightgreen)](https://tools.ietf.org/html/rfc9001)
[![](https://img.shields.io/static/v1?label=RFC&message=9002&color=brightgreen)](https://tools.ietf.org/html/rfc9002)
[![](https://img.shields.io/static/v1?label=RFC&message=9114&color=brightgreen)](https://tools.ietf.org/html/rfc9114)
[![](https://img.shields.io/static/v1?label=RFC&message=9204&color=brightgreen)](https://tools.ietf.org/html/rfc9204)
[![](https://img.shields.io/static/v1?label=RFC&message=9221&color=brightgreen)](https://datatracker.ietf.org/doc/html/rfc9221)


[![](https://img.shields.io/static/v1?label=draft-13&message=QUIC-LB&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-load-balancers-13)
[![](https://img.shields.io/static/v1?label=draft-05&message=Multipath-QUIC&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-multipath-05)
[![](https://img.shields.io/static/v1?label=draft-06&message=Multipath-QUIC&color=9cf)](https://tools.ietf.org/html/draft-ietf-quic-multipath-06)
[![](https://img.shields.io/static/v1?label=draft-07&message=QUIC-Qlog&color=9cf)](https://datatracker.ietf.org/doc/html/draft-ietf-quic-qlog-main-schema-07)

#### Standardized Features

* All big features conforming with [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000), [RFC9001](https://www.rfc-editor.org/rfc/rfc9001), [RFC9002](https://www.rfc-editor.org/rfc/rfc9002), [RFC9114](https://www.rfc-editor.org/rfc/rfc9114) and [RFC9204](https://www.rfc-editor.org/rfc/rfc9204), including the interface between QUIC and TLS, 0-RTT connection establishment, HTTP/3 and QPACK.
* ALPN Extension conforming with [RFC7301](https://www.rfc-editor.org/rfc/rfc7301)

#### Not Yet Standardized Features

* [Multipath QUIC](https://tools.ietf.org/html/draft-ietf-quic-multipath-04)
* [QUIC-LB](https://tools.ietf.org/html/draft-ietf-quic-load-balancers-13)

#### Library Features

* Pluggable congestion control: NewReno, Cubic, BBR and BBRv2, ...
* Pluggable cryptography, integration with BoringSSL and BabaSSL
* Cross-platform implementation, support Android, iOS, HarmonyOS, Linux, macOS and Windows(v1.2.0)

## Requirements

To build XQUIC, you need 
* CMake
* BoringSSL or BabaSSL

To run test cases, you need
* libevent
* CUnit

## QuickStart Guide

使用./build.sh可以快速生成交叉编译的可执行程序。

## Contributing

We would love for you to contribute to XQUIC and help make it even better than it is today! All types of contributions are encouraged and valued. Thanks to [all contributors](https://github.com/alibaba/xquic/blob/main/CONTRIBUTING.md#all-contributors). See our [Contributing Guidelines](./CONTRIBUTING.md) for more information.

If you have any questions, please feel free to open a new Discussion topic in our [discussion forums](https://github.com/alibaba/xquic/discussions).

## License

XQUIC is released under the Apache 2.0 License.

## Contact Us

Feel free to contact us in the following ways:

* e-mail: xquic@alibaba-inc.com
* Dingtalk group: 34059705
* slack channel: #xquic in quicdev group

  <img src="docs/images/dingtalk_group.JPG" width=200 alt="dingtalk group"/>
