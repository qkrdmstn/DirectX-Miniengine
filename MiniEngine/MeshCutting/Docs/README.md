# MeshCutting 문서 목차

필요한 문서만 연다. 구조를 모르면 `CodeMap.md`부터, 기법 판단은 `NDC25Reference.md`를 본다.
상태가 바뀌면 이 표와 해당 문서 머리의 `상태:` 줄을 함께 고친다.

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `NDC25Reference.md` | 원본 기법(NDC25) 정리, 항목별 근거 수준 | 기준 문서 | 설계·구현·리뷰 판단 전 |
| `CodeMap.md` | 계층, 호출 흐름, 상태의 주인 (줄 번호 없음) | 기준 문서 | 코드를 처음 열 때, 수정 위치를 찾을 때 |
| `ClassLayoutDesign.md` | 기능을 클래스 6개로 묶는 계획 (K-0~K-6) | 설계안 | 클래스 정리 작업 전 |
| `PerSegmentCutDesign.md` | 선분별·인스턴스별 절단 작업 | 구현 진행 중 (`b606ba4`), 단계는 10절 | 절단 작업 단위를 바꿀 때 |
| `PlaneFirstCutDesign.md` | world 유한 평면, 후보 패치 (P-1, P-2) | P-1·P-2 완료, P-3 폐기 | 평면·후보 패치 코드를 고칠 때 |
| `SceneViewControlsDesign.md` | 마우스 카메라, 평면 기즈모, 키 (S-1~S-4) | 완료 (`0fc39fe`), 현재 조작의 기준 | 입력·기즈모를 고칠 때 |
| `CutDebugDesign.md` | 디버그 와이어프레임, 미리보기/확정 절단 (D-0~D-3) | 완료 (`d33b1a7`) | 디버그 표시를 고칠 때 |
| `PlaneSlicingDesign.md` | 평면 슬라이스, 루프, cap, 조각 스키닝 알고리즘 | 완료 (`3e0f55f`, `11c4be6`) | Slice/SliceCap/Skinning 알고리즘을 볼 때 |
| `archive/ProjectFlowReport.md` | 2026-10-20 시점 흐름 보고서 (Codex 작성) | 보관, 줄 번호 낡음 | 과거 구조를 확인할 때만 |
