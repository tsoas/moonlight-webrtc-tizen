# Third-party notices

This inventory was audited against the current Windows release staging output
and `build-vcpkg-release/vcpkg_installed/vcpkg/info`. The listed versions are
the versions used to build the current beta artifacts, not a promise for later
releases.

## Important release blocker

Moonlight WebRTC includes `third_party/moonlight-common-c`, whose included
licence is GNU GPL version 3. The repository does **not** currently declare a
top-level licence for Moonlight WebRTC itself. A maintainer must establish an
appropriate compatible project licence and complete GPL distribution
requirements before public distribution. In particular, an executable-form
distribution must provide the corresponding source and the required GPL notice
and licence information. This notice does not select a licence or provide a
legal conclusion.

The current Windows installer does not yet package this notice document as a
separate installed file. That distribution step must be resolved as part of
the licence-compliance work; it is intentionally outside this documentation
and release-preparation-only milestone.

## Windows runtime inventory

The current installer stages the following dynamic runtime libraries:

| Component | Version | Distributed file(s) | Licence / notice source |
| --- | --- | --- | --- |
| curl | 8.21.0 | `libcurl.dll` | [curl licence](https://curl.se/docs/copyright.html) |
| OpenSSL | 3.6.3 | `libcrypto-3-x64.dll`, `libssl-3-x64.dll` | [Apache-2.0](https://www.openssl.org/source/license.html) |
| libdatachannel | 0.24.5 | `datachannel.dll` | [MPL-2.0](https://github.com/paullouisageneau/libdatachannel/blob/master/LICENSE) |
| libjuice | 1.7.2 | `juice.dll` | [MPL-2.0](https://github.com/paullouisageneau/libjuice/blob/master/LICENSE) |
| libsrtp | 2.8.0 | `srtp2.dll` | [BSD-3-Clause](https://github.com/cisco/libsrtp/blob/master/LICENSE) |
| pugixml | 1.16 | `pugixml.dll` | [MIT](https://github.com/zeux/pugixml/blob/master/LICENSE.md) |
| zlib | 1.3.2 | `z.dll` | [zlib licence](https://zlib.net/zlib_license.html) |

The following dependencies are linked into the Gateway rather than copied as
separate runtime DLLs, and remain subject to their notices:

| Component | Version | Licence / notice source |
| --- | --- | --- |
| moonlight-common-c | pinned submodule | GPL-3.0; [included licence](third_party/moonlight-common-c/LICENSE.txt) |
| ENet (inside moonlight-common-c) | bundled | MIT; [included licence](third_party/moonlight-common-c/enet/LICENSE) |
| nanors (inside moonlight-common-c) | bundled | MIT; [included licence](third_party/moonlight-common-c/nanors/LICENSE) |
| nlohmann-json | 3.12.0 | [MIT](https://github.com/nlohmann/json/blob/develop/LICENSE.MIT) |
| usrsctp | 0.9.5.0 | [BSD-3-Clause](https://github.com/sctplab/usrsctp/blob/master/LICENSE.md) |
| plog | 1.1.11 | [MIT](https://github.com/SergiusTheBest/plog/blob/master/LICENSE) |

`VC_redist.x64.exe` is also bundled by the Windows installer. It is the Visual
Studio-supplied Microsoft Visual C++ x64 Redistributable (current staged
version: 14.51.36247.0) and is redistributed under Microsoft's applicable
Visual Studio redistribution terms.

The Samsung Tizen WGT contains the project's HTML, JavaScript, CSS, assets,
and normal package-signature metadata; it does not bundle the above Windows
runtime DLLs.

## Required follow-up before public distribution

1. Declare a compatible licence for Moonlight WebRTC at repository root.
2. Confirm the corresponding-source offer/delivery mechanism for the GPL-3.0
   component and the combined work.
3. Include the applicable licence texts and notices with each executable
   distribution, including the Windows installer payload or an equivalent
   accompanying distribution.
4. Re-audit the list whenever the vcpkg baseline, staged DLLs, or submodule
   revision changes.
