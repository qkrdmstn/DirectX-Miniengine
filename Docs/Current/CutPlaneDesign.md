상태: 완료 — 유한 평면 단계 1·2·3 구현·리뷰 완료 (기준 HEAD `527ed749`, 미커밋 문서 정리; 구현 변경 없음)

# 유한 절단 평면

현재 Static GPU 절단은 [StaticCutImplementation.md](../GPU/StaticCutImplementation.md)를 따른다. 아래는 평면 기하·편집·표시 단계의 구현 이력이다. 유한 사각형이 전체 절단 경계를 덮지 못하면 절단 후보 전체를 거절하며 부분 절개는 하지 않는다.

`CutPlaneDesc`는 로컬 XZ, y=0의 Width/Height를 정의한다. `TryBuildCutPlaneData`는 affine 역변환과 +Y 역전치 법선을 사용해 `CutPlaneData`를 만든다. 반사·비균일 스케일·전단은 허용하고 비유한 크기·특이 변환은 거절한다. WorldPlane은 정규화된 `(normal.xyz, d)`이다.

`IntersectCutPlaneRectangle`은 정규화된 월드 ray를 받아 경계를 포함하며 월드 거리 t를 보존한다. 평행·공면 ray와 뒤쪽 교점은 거절한다. `ClipCoplanarSegmentToCutPlaneRectangle`은 양 끝이 외부인 통과 선분도 [0,1] 구간으로 잘라낸다. 공면 허용 오차는 로컬 Y의 1e-5이다. 실패 시 출력 인자는 보존한다.

순수 기하 API와 ModelViewer의 선택·편집·표시를 구현했다. 독립 CPU 검증은 `tests/CutPlaneTests.cpp`에서 수행하며 라이브러리에는 main을 등록하지 않는다. 실제 GPU 절단·교차 미리보기·유한 영역 일부만 걸친 메시의 절단 정책은 아직 구현하지 않았으며 NDC25 기법 전체의 GPU 구현 완료를 의미하지 않는다.

단계 1 검증: CPU 24개 assertion 통과, ModelViewer 솔루션 Debug/Release 빌드 통과, 독립 코드 리뷰 수정 후 재검토 승인.

단계 2: `Viewer::CutPlaneSystem`은 모델 없는 GameObject의 ID에 크기 설명을 연결한다. `BuildData`는 현재 객체 TRS를 사용하고 `Find`는 이미 삭제된 객체를 거절한다. `PruneStaleObjects`는 프레임 시작과 장면 정리 시 설명을 제거한다. 크기 핸들은 공통 axis mask로 XZ만 허용하며 중앙 핸들도 Y를 보존한다. 위치·회전 및 TRS Undo는 기존 기즈모 경로를 사용하고 설명 크기 변경 Undo는 제공하지 않는다. 모델 선택은 기존 경계 구 근사이며 평면 선택은 유한 직사각형으로 계산해 월드 t가 가장 가까운 후보를 고른다.

단계 2 검증: SceneMathTests 457개 검사 통과, Debug/Release 빌드 통과, 독립 코드 리뷰 승인.

단계 3: `RenderCutPlanes`는 `BuildData`의 네 월드 모서리에서 테두리와 10등분 격자(평면당 22개 선)를 만든다. 정점은 16바이트 정렬·32바이트이며 전용 VS/PS와 CBV root/line PSO를 사용한다. 메인 투명 메시 뒤, 후처리 전에 HDR 장면 컬러와 read-only scene depth를 바인딩하고 reversed-Z GREATER_EQUAL 검사·깊이 쓰기 금지로 그린다. 모델처럼 그림자/스키닝 대상에 넣지 않는다. 초기 선택 없이 원점에 기본 청록색 4×4 XZ 평면 하나를 만들며 클릭 내부는 선택되고 선택 테두리는 노란색이 된다. 교차 미리보기와 실제 GPU 절단은 범위 밖이다.

단계 3 검증: 독립 렌더·셰이더·등록 리뷰 승인. 최종 실행 검증은 아래 인계 기록을 따른다.

## 최종 검증 기록과 한계

다음 수치는 상위(root) 작업 세션에서 실행하고 인계한 결과다. 이 문서 정리 세션에서는 빌드·CPU 테스트·GPU 실행을 재수행하거나 원본 로그를 다시 판독하지 않았다.

- CPU 기하 검증: `CutPlaneTests.cpp`의 24개 assertion 통과.
- Scene 수학 검증: `SceneMathTests.cpp`의 457개 check 통과.
- 최종 ModelViewer 솔루션 빌드: Debug 12.72초, Release 10.26초, 모두 exit 0.
- 원래 Debug `ModelViewer.exe`를 `-debug 1 -view 0`, `-debug 1 -view 1`, `-debug 1 -view 2`로 각각 45초 실행한 디버거 캡처에서 시작 모드, PSO 준비, 평면 draw 제출(planes 1, lines 22), 첫 장면 GPU 작업 완료를 모두 확인했다. fatal/assert는 없었다.
- 인계상 ShadowPlay error 87은 드라이버의 별도 로그이며 D3D12 오류로 분류하지 않았다. GPU 모델·드라이버 버전과 원본 로그 경로는 이 인계에 포함되지 않아 여기서 특정하지 않는다.

GPU 로그는 제출·완료의 근거다. 화면 색·가림·실제 마우스 드래그의 수동 시각 검증은 하지 못했다. 실제 GPU 절단·교차 미리보기·유한 영역 일부만 걸친 메시의 절단 정책은 미구현이다.

## 현재 조작 계약

`Q` 선택(View), `W` 이동, `E` 회전, `R` 크기를 사용한다. `L`은 이동·회전의 World/Local을 전환하고 크기는 항상 Local이다. 평면 크기 편집은 X/Z만 바꾼다. `Ctrl+Z`는 완료 드래그를 취소하고 드래그 중 `Esc`는 시작 TRS로 복구한다. 오른쪽 버튼을 유지하면 마우스로 카메라를 회전하고 WASD/QE로 이동한다. F 프레임 키는 없다. 상세 계약은 [Scene.md](Scene.md)를 따른다.
