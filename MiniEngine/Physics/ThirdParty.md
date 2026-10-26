# Jolt Physics

- Version: v5.6.0
- Commit: e77f175595e64cb44218cc9d9d56fc365ad0e36a
- Source: https://github.com/jrouwe/JoltPhysics
- Archive SHA256: 1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
- License: MIT, Copyright 2021 Jorrit Rouwe

`Tools/BuildJolt.ps1` downloads and verifies the pinned source. Source, build
products and the upstream `LICENSE` remain under ignored `MiniEngine/Build/ThirdParty/Jolt/`.
The adapter inherits Jolt's public definitions through its CMake target.
Debug uses /MDd; Release and Profile use /MD. The initial x64 CPU baseline is SSE2.

## MIT License

Copyright 2021 Jorrit Rouwe

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
