# 주말 Codex 작업 후보 (2026-10-23 조사, 읽기 전용)

조사: Sonnet 서브에이전트. 근거는 현재 `Docs/`로 이동한 문서, 코드, git 기록. 코드 주석의 TODO는 사실상 없고 예정 작업은 거의 전부 문서에 있다.

## 먼저 알아둘 것
- Geometry fail-soft cap, OpenSurface 태그, BreakConstraint 서브트리 고정은 이미 커밋됨 (`548fa07`, `fc1f739`). 그런데 `UnrealSourceComparison.md` 상태 줄은 아직 "커밋 전".
- 미커밋 변경은 Enter 비용 단계 A(스태틱 용접 결과 캐시) 코드, 문서 3개, `PhysicsSkeletalCutDesign.md`(새 파일), `Deliverables/측정/enter_probe_stepA_*.log` 7개.
- Codex P2(모델 교체 감지 없음)는 아직 열려 있음. `CutController::UpdateTarget`이 `target.model`과 비교하지 않는다. 문서상 계약은 "Unregister 후 Register"이고 ModelViewer는 지킨다.

## Codex 단독에 맞는 순서
| 순서 | 작업 | 근거 | 크기 | 자체 검증 |
| --- | --- | --- | --- | --- |
| 1 | 문서 상태 줄 정리: `UnrealSourceComparison.md`("코드 구현 완료, 화면 확인 대기"), README 표, `CutEnterCostReport.md`("개선 미구현"), `EnterCostReductionDesign.md`/README의 "A 설계안" 불일치 | 각 문서 1행, README | 작음 | 문서만 |
| 2 | `UpdateTarget`에 `instance->GetModel() != target.model` 방어(건너뛰기 + 오류 문자열) + `ExtractJob` 실패 시 `job.source` 비우기 | `Runtime/CutController.cpp`, `EnterCostReductionDesign.md` 5절 | 작음 | Debug/Release 빌드 |
| 3 | 테스트 보강: `Slicer::CapWhatFits` 묶음별 재시도 경로, 단계 A 캐시 경로와 직접 경로 결과 동일성 | `tests/PlaneSlicerTests.cpp`, `EnterCostReductionDesign.md` 4절 | 작음~중간 | CPU 테스트 |
| 4 | 안 D: `CollectEdgeUsage` · corner 매핑 해시맵 → 배열 (결과 비트 동일) | `EnterCostReductionDesign.md` 6절 | 중간 | CPU 테스트, 시간 측정은 사용자 |
| 5 | 물리 설계 R-2 · R-3 (`Runtime/` 안의 동작 불변 리팩터) 또는 대안으로 프레임 드랍 안 3(`UpdatePose` 복사 제거, `ValidateVertices` 축소) | `PhysicsSkeletalCutDesign.md` 7~9절, `CutFrameDropReport.md` 안 3 | 중간 | 빌드·테스트, 효과·화면은 월요일 |

## Codex 단독에 맞지 않는 것 (화면 확인·설계 결정·설치 필요)
- 검증: 프레임 드랍 해결안 1·2 재측정(16:18 빌드 측정 무효), bunny 바닥 구멍 절단 화면, BreakConstraint 서브트리 화면, `JointCutDesign.md` 화면 확인, `PlaneSlicingDesign.md` 2-4/2-5 사용자 확인 줄
- 기능: 평면과 겹친 면 처리(`CoplanarFace`), 짧은 에지 접기, `CrossesCap` 재절단, 조각 셰이더 조명·법선 갱신, 독립 cap 본 부착, RegionCut(합친 뒤 자르기 · 아일랜드 · 각도 보정)
- 물리: R-0(엔진 `ModelInstance` 변경, 화면 확인 필요), P-0 PhysX 설치(승인 필요), P-3, N-1
- 정리: 단계 A 커밋 단위와 측정 로그 커밋 여부(사용자 결정)
