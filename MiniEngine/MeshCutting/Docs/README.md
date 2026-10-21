# MeshCutting 문서 목차

필요한 문서만 연다. 구조를 모르면 `CodeMap.md`부터, 기법 판단은 `NDC25Reference.md`를 본다.
상태가 바뀌면 이 표와 해당 문서 머리의 `상태:` 줄을 함께 고친다.

| 문서 | 용도 | 상태 | 언제 읽나 |
| --- | --- | --- | --- |
| `NDC25Reference.md` | 원본 기법(NDC25) 정리, 항목별 근거 수준 | 기준 문서 | 설계·구현·리뷰 판단 전 |
| `CodeMap.md` | 계층, 호출 흐름, 상태의 주인 (줄 번호 없음) | 기준 문서 | 코드를 처음 열 때, 수정 위치를 찾을 때 |
| [CurrentFlowReport.md](CurrentFlowReport.md) | 현재 선분별 작업의 데이터 흐름, 타입·객체·함수·파일 역할, 중복과 개선 조언 | 분석 완료 (`1287cb5` 기준) | 최신 실행 구조를 처음부터 이해할 때 (CodeMap의 구 흐름과 구분) |
| `ClassLayoutDesign.md` | 기능을 클래스 6개로 묶는 계획 (K-0~K-6) | 설계안 | 클래스 정리 작업 전 |
| `PerSegmentCutDesign.md` | 선분별·인스턴스별 절단 작업 | 구현 (`b606ba4`). 작업 식별은 JointCutDesign으로 대체됨 (선분 key는 모양만 남김) | 작업 목록·등록·Enter/Ctrl+Z·표시를 고칠 때 |
| `JointCutDesign.md` | 메시 교차 기반 단일 joint 작업 (NDC 초기 단계, 한계 체험) | 구현 완료 (J-1~J-3, 커밋 전). 화면 확인 대기 | 무엇을 자를지(작업·패치 joint) 바꿀 때 |
| `RegionCutDesign.md` | 영역 작업, 걸친 joint 합친 뒤 자르기 (NDC 한계 돌파) | 설계안, JointCutDesign 다음 단계 | 한계 돌파 작업 전 |
| `PlaneFirstCutDesign.md` | world 유한 평면, 후보 패치 (P-1, P-2) | P-1·P-2 완료, P-3 폐기 | 평면·후보 패치 코드를 고칠 때 |
| `SceneViewControlsDesign.md` | 마우스 카메라, 평면 기즈모, 키 (S-1~S-4) | 완료 (`0fc39fe`), 현재 조작의 기준 | 입력·기즈모를 고칠 때 |
| `CutDebugDesign.md` | 디버그 와이어프레임, 미리보기/확정 절단 (D-0~D-3) | 완료 (`d33b1a7`) | 디버그 표시를 고칠 때 |
| `PlaneSlicingDesign.md` | 평면 슬라이스, 루프, cap, 조각 스키닝 알고리즘 | 완료 (`3e0f55f`, `11c4be6`) | Slice/SliceCap/Skinning 알고리즘을 볼 때 |
| `archive/ProjectFlowReport.md` | 2026-10-20 시점 흐름 보고서 (Codex 작성) | 보관, 줄 번호 낡음 | 과거 구조를 확인할 때만 |
