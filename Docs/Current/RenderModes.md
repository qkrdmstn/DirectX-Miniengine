상태: 완료 — `527ed749` 기준 미커밋 렌더 수정 (2026-10-24). 검증 결과는 아래 참조.

# ModelViewer 표시 모드

`V`는 Solid → Wireframe → Vertices → Solid로 순환하며 overlay에 현재 모드를 표시한다.
`-view 0|1|2`는 시작 모드만 선택한다. 생략·범위 밖은 Solid다.
매 프레임 `ModelViewer::RenderScene`이 현재 모드로 `MeshSorter`를 생성하므로 실행 중 전환도 같은 PSO 경로를 사용한다.

## 렌더 계약

- Solid는 원래 재질·조명·깊이 설정을 유지한다. 깊이·그림자는 모든 모드에서 solid다.
- Wireframe은 원래 VS·스키닝·양면·알파 정책을 유지하고 흰색 선을 출력한다.
  `Renderer::GetPSO`의 선 PSO는 원래 인덱스 쌍을 공유하지만 둘 다 역 Z `GREATER_EQUAL`, 깊이 쓰기 금지다.
  선/면 커버리지·깊이 보간 차이를 보정하려고 DepthBias=2, SlopeScaledDepthBias=1, DepthBiasClamp=0.00001을 사용한다.
  큰 깊이 차이는 여전히 차폐되며, 이 허용치보다 가까운 면끼리는 선이 비칠 수 있다.
- `MeshSorter::AddMesh`와 `RenderMeshes`는 Separate Z Pass가 꺼져 있어도 선·점의 불투명 solid prepass를 보장한다.
  깊이 검사를 끄거나 뒤쪽 선·점을 xray로 그리지 않는다.
- Vertices는 원본 인덱스가 참조하는 정점을 POINTLIST로 그리고 GS가 약 3px 사각형으로 확장한다.
  원본 VS의 스키닝 → World → ViewProj 계산과 tangent/UV1별 GS 입력 레지스터를 유지한다.
  공통 b1(ALL)의 c11.zw에 viewport 역수를 전달하며 b0 메시 상수는 VS 전용이다.
  점은 흰색, 역 Z `GREATER_EQUAL`, 깊이 쓰기 금지다. 경사진 면에서 사각형 일부가 잘리고 실루엣 밖으로 약 1.5px 나올 수 있다.
  중복 인덱스·UV seam 정점은 겹친다. Cube처럼 정점이 적은 모델의 면 내부는 대부분 검은 clear 색으로 남는다.
- 두 흰색 PS는 base-color alpha×factor, alpha-test discard 및 premultiplied alpha blend를 보존한다.
  점의 alpha는 정점 UV의 mip 0 샘플로 근사한다. 반투명 메시는 기존 정렬을 유지하고 불투명 깊이에 대해서만 검사한다.
- `ModelViewer::RenderScene`은 solid prepass 이후 skybox를 컬러 선·점보다 먼저 그린다.
  뒤에 skybox를 그리면 깊이를 쓰지 않은 실루엣 가장자리가 배경으로 덮인다.
  Cut Plane·Gizmo는 기존 색상과 경로를 유지한다. TAA·모션 블러는 끄고 FXAA를 사용한다.

## 픽셀 검증과 재현

ModelViewer를 작업 디렉터리로 실행한다. `-debug 1 -view 1 -capture <외부 경로 prefix>`는
60번째 프레임에 `<prefix>-hdr.raw`와 `<prefix>-post.raw`를 한 번씩 저장한다.
상위 폴더는 미리 만들어야 한다. 기본 실행에는 파일 쓰기와 추가 GPU 대기가 없다.
`PixelBuffer::ExportToFile`을 사용하고 COPY_SOURCE 상태를 shader resource 상태로 복원한다.
HDR는 RenderScene 완료 시점, post는 PostEffects 이후 RenderUI 시작 시점의 scene color다.
post는 최종 swapchain/UI 스크린샷이 아니다. typed UAV 미지원 경로도 PostEffects에서 scene color로 복사한 뒤 캡처한다.
파일은 DXGI format / RowPitch(bytes) / Width / Height의 16바이트 헤더와 raw footprint다.
R11G11B10_FLOAT는 4bytes/pixel이며 행 padding을 따라 해석한다.

2026-10-24 Debug/Release 빌드 성공. NVIDIA GeForce MX450, 드라이버 32.0.15.9186에서
`-debug 1`로 1920×1080 HDR/post raw를 실제 읽었다 (typed UAV 지원).
Wireframe은 HDR의 흰 픽셀(최솟값 >0.5, 채널 차이 <0.02)이 146 → 29,028로 늘고 삼각형 선이 복구됐다.
Vertices의 Bunny 점은 수정 전에도 출력됐으며 skybox 순서를 바꾼 뒤 Cube 실루엣 정점도 남는다.
PNG의 HDR 미리보기는 선형 HDR를 [0,1]로 clamp 후 sRGB로 변환하고, post 미리보기는 이미 톤매핑된 scene color를 sRGB로 변환한다.
검증 자료·실행 로그는 저장소 밖 `%TEMP%/render-fix/`에 둔다. GPU 완료 marker나 실행 생존만으로 픽셀 정상이라 판단하지 않는다.

Solid 최종 Release의 HDR/post raw는 수정 전 Debug와 모두 byte-identical이다.
임시 자동화 분기로 실행 중 0→1→2→0 전환과 Separate Z Pass=false를 검사했고, 처음·복귀 Solid HDR가 byte-identical이다.
이 테스트 분기는 제거했다. 실제 V 키 입력·스킨 애니메이션 모델의 수동 시각 검증은 별도 미확인이다.
Debug의 기존 필터가 숨기는 D3D12 #974/#1008 및 DXGI #80 진단은 Release 실행 로그에 기록됐다.
독립 Luna 리뷰에서 렌더·알파·상태 복원·최종 PNG를 검토했고 blocker는 없었다.
