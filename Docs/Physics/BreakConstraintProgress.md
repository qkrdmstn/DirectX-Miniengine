상태: 진행 중 — BC-07 `8dc58b08` 이후 BC-08a-1/2·BC-08b 미커밋 변경 분류·재검증 완료, BC-10 초안은 분리 표시 (2026-10-30)

# BreakConstraint 진행 기록

| 단계 | 커밋 | 검증 결과 | 리뷰 |
| --- | --- | --- | --- |
| BC-02 | 미커밋: 전체 빌드 차단으로 완료 보류 | CPU 32/32 PASS (6.60초); Debug Jolt 외부 junction 캐시 쓰기 Permission denied/MSB3073; Release·실행 미수행; master 병합 미수행 | 자기 리뷰, 유효 지적 0건; diff --check PASS |

| BC-02 재개 | 다음 단계 커밋 때 기입 | CPU 32/32 PASS(3.59초), Debug/Release 빌드 PASS(종료 코드 0), viewer 미실행(화면 경로 변경 없음) | codex exec 홈 탐색 실패로 자기 리뷰, 유효 지적 0건 |

| BC-02 커밋 차단 | 미커밋 | git add/commit 모두 worktree 외부 Git 관리 경로 index.lock Permission denied; master 병합·후속 단계 미수행; 최종 보고 BreakConstraintReport.md 저장 | 자기 리뷰, 유효 지적 0건 |

| Git 허용 후 재시도 | 미커밋, HEAD d044de76 | add/commit index.lock·merge ORIG_HEAD.lock 모두 Permission denied; ACL에 Deny Write 확인; 코드 불변으로 CPU/빌드 재실행 없음, 기존 PASS 유지 | 자기 리뷰 0건 유지, 보고서 갱신 |

## 3bee0b20 이후 단계별 변경 기록

사용자 지시로 이 실행에서는 git commit/merge를 시도하지 않는다. 단계별 파일과 검증 결과를 기록하여 나중에 나눠 커밋한다.

| 단계 | 바뀐 파일 목록 | 테스트·빌드·실행 결과 | 리뷰 지적 수 |
| --- | --- | --- | --- |
| BC-02·master 병합 인계 | 사용자 커밋 619022e6·3bee0b20 | 사용자 Debug 빌드 성공 인계; 본 세션 SkeletonPose 32/32, SceneMath 495 checks, Physics Debug 9/9 targets PASS(CTest 1.34초) | BC-02 이전 자기 리뷰 0건; 병합 충돌은 사용자 해결 |
| Static 전제 대조 | Docs/Physics/BreakConstraintImplementation.md | PhysicsWorld·PhysicsSystem·편집/삭제 경로 대조; owner 잠금·삭제 정리 연결 지점 보완 | 코드 변경 없음 |
| BC-03a | MiniEngine/Model/Model.h, Model.cpp; MiniEngine/ModelViewer/Scene/tests/XbotArmChecks.h; 계약·README·CodeMap·Progress | CPU 회귀 32/32·495 checks·9/9; Debug 10.94초/Release14.75초 PASS; Debug Xbot 실행6.32초, BC-03a·BC-00 PASS; synthetic 중복2개 확인 | 별도 새 Codex 읽기 전용 리뷰 0건 |

| BC-03b | MiniEngine/ModelViewer/Scene/Scene.h, Scene.cpp, tests/MeshComponentChecks.h; 계약·README·Scene·CodeMap·Progress | CPU32/32·495 PASS; Debug136.80초+최종로그빌드 PASS/Release177.00초 PASS; physics-sim0 컴포넌트 Debug4.67초·Release4.66초 PASS. 물리기본1로 잘못 실행한 초기 검사 실패는 계약에 기록 | 별도 새 Codex 0건 |
| BC-03b 리뷰 수정 | MiniEngine/ModelViewer/Scene/tests/MeshComponentChecks.h, Scene.cpp, Scene.h; Docs/Current/Scene.md; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md | CPU SkeletonPose32/32 PASS 1.06초·SceneMath495 PASS 1.03초(컴파일+실행); Debug 빌드 PASS 최초39.40초·최종18.26초, Release PASS 최초162.27초·최종5.14초; 컴포넌트 Debug PASS 11.30초·Release PASS 6.19초; Debug Xbot BC-03a·BC-00 PASS 8.13초; 모든 최종 종료 코드0 | 지적 3건 처리: 4개 dt의 wrapper/분리 경로 전체 node·position/normal 비교 및 GPU 준비 전후 보존, FinalizePoses 두 줄 주석, Scene 소유권 본문 동기화 |
| BC-04a（커밋 8593a0be） | MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.h, SkeletalPhysicsAsset.cpp; MiniEngine/Physics/tests/SkeletalAssetTests.cpp; MiniEngine/Physics/CMakeLists.txt, Physics.vcxproj, Physics.vcxproj.filters; Tools/BuildJolt.ps1; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | CPU 42/42·/O2 /DNDEBUG 42/42 PASS, 종료0; Debug/Release 최초22.51/7.44초·최종6.89/7.41초 PASS, 경고0·오류0; CTest·viewer 미실행（호출 경로 변경 없음） | codex exec 홈 탐색 실패로 자기 리뷰; 매크로 한 줄 여러 문장·weld 누락 2건 수정 후 재검증, 잔여0건 |

BC-04a 산출물·로그는 `$env:TEMP\BreakConstraintTests`에 두었다(`SkeletalAssetTests.exe`, `SkeletalAssetTestsRelease.exe`, `BC04a-Debug-build-final.log`, `BC04a-Release-build-final.log`, `BC04a-review.log`). 새 C++ 파일은 총 275줄이며 실제 fixture·Xbot 생성 API는 추가하지 않았다. 기존 .mini 3개와 BreakConstraintReport.md는 수정하지 않았고 파일 삭제·junction 수동 변경·git commit/merge/stash/reset/checkout은 수행하지 않았다.

### BC-03b 리뷰 수정 검증 조건 (2026-10-29)

CPU 명령은 SkeletonPoseTests.cpp와 SceneMathTests.cpp 머리말을 따랐으며 산출물과 검증 로그는 `$env:TEMP\BreakConstraintTests`에 저장했다. worktree 루트에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows`와 Configuration=Release를 실행했다(/restore 없음). 빌드의 기존 C4819 경고와 VS 환경 초기화의 vswhere 메시지는 남아 있으나 최종 빌드는 모두 오류0이다.

MiniEngine/ModelViewer에서 Debug·Release 각각 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`, Debug에서 `-debug 1 -xbot-arm-checks 1`로 실행했다. 두 컴포넌트 로그 모두 3개 owner × 4개 dt의 wrapper 동등성12건·PrepareMeshes 보존12건과 최종 PASS GPU preparation을 확인했다. DirectX debug layer를 활성화했고 GPU 기반 검증 옵션은 사용하지 않았다. 드라이버 레지스트리의 NVIDIA GeForce MX450 버전은 32.0.15.9186이며 실제 선택 adapter는 별도 계측하지 않았다(WMI 조회는 접근 거부).

수정 도중 첫 Debug 컴포넌트 실행은 41.75초 후 종료 코드 -1073741819로 실패했다. 새 진단의 임시 Scene이 미제출 GPU 명령의 버퍼를 먼저 해제한 문제를 `context.Flush(true)`로 수정하고 위 최종 빌드·실행을 통과했다. BC-03a/03b 변경 diff에서 같은 줄의 두 문장 주석은 FinalizePoses 선언·정의 두 곳뿐임을 확인하고 모두 나눴다. BC-04 이후, MiniEngine/Physics 코드, .mini, BreakConstraintReport.md는 수정하지 않았으며 커밋·병합·stash·checkout을 수행하지 않았다.

### BC-04b 완료 (2026-10-29)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-04b | `d31863a7` | MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.h, SkeletalPhysicsAsset.cpp; MiniEngine/Physics/tests/SkeletalAssetTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | 직접 cl CPU 82/82 PASS 1.19초, /O2 /DNDEBUG 82/82 PASS 3.37초, 경고 없음; ModelViewer Debug 105.21초·Release 119.91초 PASS, 종료0, /restore 없음; 기존 C4819·vswhere/pwsh 탐색 메시지 있음; CTest·viewer 미실행(새 API의 런타임 호출 없음) | 자기 리뷰, anchor 시험 부족 1건 보강 후 재검증, 잔여0건; diff --check PASS |

fixture 3 body·2 joints·body 없는 tip, Xbot 고정 NodeId 13→14→15·명시적 m/kg/rad 값을 추가했다. GLB 정적 translation을 읽어 +X 방향을 확인하고 BC-00 배율에서 반올림 anchor 오차≤0.1mm를 검사했다. 손목 swing/twist 제한은 BC-00 측정 범위를 포함한다. 손 크기·폭·두께·질량·관절 한계는 M1 proxy 값이며 계약 §6·§8에 근거와 한계를 적었다. 새 파일이 없어 기존 vcxproj/.filters·CMake·BuildJolt.ps1 시험 target 등록을 그대로 사용한다.

CPU 산출물과 빌드 로그 및 자기 리뷰 메모는 `$env:TEMP/BreakConstraintTests`에 있다(`SkeletalAssetTests.exe`, `SkeletalAssetTestsRelease.exe`, `BC04b-Debug-build.log`, `BC04b-Release-build.log`, `BC04b-self-review.txt`). BC-04a 커밋 칸을 `8593a0be`로 채웠다. BC-05 이후는 시작하지 않았고 git 쓰기 명령·파일 삭제·junction 수동 변경·기존 .mini 3개 및 BreakConstraintReport.md 편집은 하지 않았다.
### BC-05 완료（84ecea1f） (2026-10-29)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-05 | `84ecea1f` | MiniEngine/Physics/PhysicsWorld.h, PhysicsWorldBodies.cpp, tests/PhysicsBodyTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | CPU Debug 362/362 PASS 7.63초·Release 362/362 PASS 10.26초, 경고0; 전체 Debug 40.82초·Release 136.18초 PASS, 오류0, 기존 경고18/20; 물리OFF component-checks Debug 6.32초·Release 7.31초 PASS GPU preparation·종료0; CTest 전체 미실행 | 자기 리뷰, 왕복 전환/거절 뒤 속도 보존 시험 부족1건 보강, 재검증 후 잔여0건; diff --check PASS |

BC-04b 커밋 칸을 `d31863a7`로 채웠다. 기존 handle·COM 속도 조회·Moving 계층을 재사용하고 Kinematic 생성·MoveKinematic·Dynamic 왕복 전환·속도 설정·월드 작용점 impulse를 추가했다. 새 파일이 없어 기존 PhysicsBodyTests의 CMake·BuildJolt.ps1 testTargets·vcxproj/.filters 등록을 재사용한다. API와 시험 C++ 변경은 약 250줄이다.

검증 산출물과 로그는 `$env:TEMP/BreakConstraintTests`의 `PhysicsBodyTestsDebug.exe`, `PhysicsBodyTestsRelease.exe`, `BC05-CPU-*-final.log`, `BC05-*-build-final.log`, `BC05-*-component-checks.log`, `BC05-self-review.txt`다. 실행 명령은 ModelViewer 폴더에서 각 구성 exe에 `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`이다. DirectX debug layer 사용, GPU/driver 재측정 없음이다. 첫 CPU/Debug 컴파일의 벡터 선언 오류·시험의 Dynamic 전용 질량 조회 오류·재검증 스크립트의 LNK1181은 수정 후 최종 재검증했다(계약 §12). 초기 실패를 성공으로 계산하지 않았다.

BC-05만 끝내고 중단한다. BC-06 이후 미착수이며 git commit/merge/stash/reset/checkout·파일 삭제·junction 수동 변경·.mini 수동 편집·BreakConstraintReport.md 편집은 하지 않았다. 기존 .mini 부산물은 그대로 커밋 대상에서 제외한다.
### BC-06 완료·미커밋 (2026-10-29)

독립 Codex 리뷰 지적 1건(중간: disable 시 endpoint 미활성화) 수정. `PhysicsWorld::SetConstraintEnabled`는 enable/disable 모두 양쪽 endpoint 중 Static을 제외하고 활성화한다. 이번 수정 파일은 `MiniEngine/Physics/PhysicsWorldConstraints.cpp`, `MiniEngine/Physics/tests/PhysicsConstraintTests.cpp`, 이 진행 문서다.

회귀 시험 `CheckSleepingEndpointFalls`는 Dynamic이 Parent인 경우와 Child인 경우를 각각 검사한다. Kinematic에 매단 Dynamic을 강제 재우지 않고 step하여 자연 수면을 확인한 뒤, 관절 disable 직후 활성화와 다음 step의 위치 하강을 확인한다. 파일 머리말의 직접 cl 명령으로 CPU Debug 286/286 PASS(23.79초), Release 286/286 PASS(32.24초), 모두 경고0·종료0이다. 요청한 전체 solution msbuild는 /restore 없이 Debug PASS(9.29초), Release PASS(8.53초), 모두 경고0·오류0·종료0이다. 두 구성 모두 수정한 `PhysicsWorldConstraints.cpp` 재컴파일을 확인했으며 기존 산출물을 사용하는 증분 빌드다.

재시험 CPU 산출물과 로그는 `$env:TEMP/BreakConstraintTests`의 `PhysicsConstraintTestsDebug.exe`, `PhysicsConstraintTestsRelease.exe`, `BC06-review-fix-CPU-Debug.log`, `BC06-review-fix-CPU-Release.log`, `BC06-review-fix-Debug-build.log`, `BC06-review-fix-Release-build.log`다. 초기 VS 환경 스크립트는 실행 정책으로 차단되어 DevShell 모듈로 전환했고, 첫 컴파일 호출의 인자 전달 경고는 인자 배열로 수정한 뒤 두 구성을 재검증했다. 위 수치는 최종 성공 실행만 기록한다. 이번 수정에서는 viewer 실행과 CTest 전체 실행을 하지 않았다. BC-07은 시작하지 않았다.

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-06（06a API·06b 검증） | `f6a14195` | MiniEngine/Physics/PhysicsWorldConstraints.cpp（신규）, PhysicsWorld.h, PhysicsWorldInternal.h, PhysicsWorld.cpp, PhysicsWorldBodies.cpp; tests/PhysicsConstraintTests.cpp（신규）, tests/PhysicsBodyTests.cpp（머리말 명령만）; CMakeLists.txt, Physics.vcxproj, Physics.vcxproj.filters; Tools/BuildJolt.ps1; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | 관절 CPU Debug 200/200 PASS 16.00초·Release 200/200 PASS 29.99초; 기존 body Debug 362/362 PASS 17.68초·Release 362/362 PASS 88.90초, CPU 경고0·종료0; 전체 Debug 최종129.26초·Release 최종138.98초 PASS, 각각 오류0·기존 C4819 경고18개, /restore 없음; 비물리 component-checks Debug 34.50초·Release 16.77초 PASS GPU preparation·종료0; CTest 전체 미실행 | 자기 리뷰 2건（잠든 endpoint 활성화, solver/접촉/정리 증거 부족）처리·재검증, 잔여0건; diff --check PASS; 독립 리뷰는 상위 관리자 담당 |

BC-05 커밋 칸을 `84ecea1f`로 채웠다. 공통 API와 수명 연결 약280줄, CPU 시험과 등록 약200줄을 계약 §11의 BC-06a/06b로 나누었다. SwingTwist는 body 원점 anchor를 COM 기준으로 바꾸고 +X twist/+Y plane 축·제한을 전달한다. 두 Kinematic 관절은 disabled이며 마지막 Dynamic을 Kinematic으로 바꾸면 꺼진다. Dynamic 전환 후 enable은 호출자 책임이다. 모터는 처음부터 두 종류 모두 Off이고 별도 Off API도 제공한다.

인접 pair는 등록자가 `ExcludeBodyPair`에 명시한다. 같은 rig인지 판단하고 직접 인접 pair만 넘기는 적용은 BC-07 범위다. body 수명까지 제외를 유지하므로 관절 제거 후에도 다시 접촉하지 않는다. 비인접 body·타 owner·Static 바닥의 실제 접촉, COM offset·회전축·한계, 120 step 손목 anchor 오차 <0.01m, 실패/중복/stale handle, 직접 body 제거·파괴·Clear·살아 있는 Shutdown 정리를 CPU에서 확인했다.

산출물·로그·자기 리뷰 메모는 `$env:TEMP/BreakConstraintTests`의 `PhysicsConstraintTestsDebug.exe`, `PhysicsConstraintTestsRelease.exe`, `PhysicsBodyTestsDebug.exe`, `PhysicsBodyTestsRelease.exe`, `BC06-PhysicsConstraintTests-*.log`, `BC06-PhysicsBodyTests-*.log`, `BC06-*-build.log`, `BC06-*-component-checks.log`, `BC06-self-review.txt`다. CPU 명령은 새 시험 파일 머리말을 따른다. 전체 빌드는 worktree 루트에서 `msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows` 및 Configuration=Release로 수행했다. 초기 두 구성 빌드도 79.68/79.23초에 통과했으며 완료 증거는 위 최종 빌드다. VS 환경의 기존 vswhere/pwsh fallback 메시지가 있었으나 빌드는 성공했다.

화면 회귀 명령은 MiniEngine/ModelViewer에서 `../Build/x64/Debug/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`이며 Release도 같은 인자다. 실제 판정 실행은 Start-Process -WindowStyle Hidden -Wait -PassThru로 종료를 기다리고 실행 시작 뒤 갱신된 MeshComponentChecks.log를 확인한다. 최초 Debug GUI 직접 호출은 종료를 기다리지 않아 측정·판정에서 제외하고 다시 실행했다. DirectX debug layer 사용, GPU 기반 검증·adapter/driver 재측정 없음이다.

BC-06만 끝내고 중단한다. BC-07 이후는 시작하지 않았다. git commit/merge/stash/reset/checkout·파일 삭제·junction 수동 변경·.mini 수동 편집·BreakConstraintReport.md 편집을 하지 않았다. 기존 .mini 3개와 미추적 보고서는 그대로 두며 이번 변경 파일 목록에 포함하지 않는다.
### BC-07 완료·미커밋 (2026-10-29)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-07 | 미커밋, 상위 관리자 담당 | MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.h, SkeletalPhysicsSystem.cpp（신규）, tests/SkeletalBreakTests.cpp（신규）, PhysicsSystem.h, PhysicsSystem.cpp; MiniEngine/ModelViewer/ModelViewer.vcxproj, ModelViewer.vcxproj.filters; MiniEngine/Physics/CMakeLists.txt; Tools/BuildJolt.ps1; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | 직접 cl CPU Debug 64/64 PASS 17.76초·Release 64/64 PASS 19.18초, 경고0·종료0; 전체 Debug 52.13초·Release 43.36초 PASS, 오류0·기존 C4819 19개, /restore 없음; 비물리 component-checks Debug 3.31초·Release 4.10초 PASS GPU preparation·종료0; CTest 미실행 | 자기 리뷰 잔여0건; 독립 리뷰 낮음 지적 수정(높음·중간 0건); diff --check PASS |

BC-06 커밋 칸을 `f6a14195`로 채웠다. `SkeletalPhysicsSystem`은 `PhysicsWorld&`와 `ObjectId`만 의존하며 `PhysicsSystem`이 `m_World` 뒤 멤버로 소유하고 `GetSkeletalPhysics()`로 등록자만 공개한다. 등록은 검증 → 전체 Prepare → commit → 관절(두 Kinematic이라 disabled) → 인접 pair 제외 순이며 질량은 `MassKg`가 되도록 공통 질량식 배율을 맞춘다. 실패하면 관절 역순 제거 → body 역순 파괴로 되돌린다. owner 삭제(`RemoveTarget(owner, InvalidComponentId)`)와 `PhysicsSystem::Clear`가 world 종료 전에 rig를 해제한다. body 초기 frame은 호출자 입력이며 `E*Gnode` 계산·scale 분리는 BC-08a다.

시험은 거절 4종, fixture·Xbot 등록과 공존, Kinematic frame·disabled 관절·모터 Off, Dynamic 전환 뒤 질량, 7단계 주입 실패와 용량 부족의 역정리, 재등록 새 세대, Unregister·Clear·소멸자 뒤 body·관절·pair·예약 0을 확인한다. 산출물·로그는 `$env:TEMP/BreakConstraintTests`의 `SkeletalBreakTestsDebug.exe`, `SkeletalBreakTestsRelease.exe`, `BC07-Debug-build.log`, `BC07-Release-build.log`다. CMake/CTest 경로는 `MiniEngine/Build/ThirdParty` 쓰기가 필요해 실행하지 않았다. 변경 C++는 약 330줄(구현 193·시험 124·연결 11)이다.

BC-07만 끝내고 중단한다. BC-08a 이후 미착수이며 git commit/merge/stash/reset/checkout·파일 삭제·junction 변경·.mini 수동 편집·BreakConstraintReport.md 편집은 하지 않았다.

독립 리뷰 낮음 지적 수정: 한 줄 두 문장 주석 분리(PhysicsSystem.h `GetSkeletalPhysics`, SkeletalPhysicsSystem.h 클래스·`Unregister`·`Clear`·`DiagFailRegistrationStep`)와 SkeletalPhysicsSystem.cpp `Clear`·`GetRigGeneration`·`GetBodyHandle`·`GetConstraintHandle` 본문 단락 주석 추가. PhysicsSystem 경유 해제(`RemoveTarget`·`Clear`)는 Scene 의존으로 CPU 시험에 없으며 BC-12b/BC-13a 수명 회귀에서 실제 경로를 확인한다.

### BC-08a-1 완료·미커밋 (2026-10-29, `8dc58b08` 기준)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-08a-1 | 미커밋 | MiniEngine/ModelViewer/Scene/SkeletalPhysicsFrames.h（신규）, tests/SkeletalFrameTests.cpp（신규）; ModelViewer.vcxproj, ModelViewer.vcxproj.filters; MiniEngine/Physics/CMakeLists.txt; Tools/BuildJolt.ps1; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/Current/CodeMap.md; Docs/README.md | 직접 cl CPU Debug 706/706 PASS 0.89초·Release 706/706 PASS 1.27초, 경고0·종료0; 전체 Debug 최종4.88초·Release 최종3.70초 PASS, 경고0·오류0·종료0(최초12.61/38.95초, Release 기존 C4819 19개), /restore 없음; CTest·viewer 미실행(런타임 미연결) | 자기 리뷰 affine exact 비교 문제1건 수정; 독립 리뷰 P3 1건 수정·재검증·재리뷰 잔여0건 |

BC-07은 `8dc58b08`로 커밋 완료된 상태를 확인했다. BC-08a를 순수 좌표 수학과 rig 적용으로 나누었다. 이번 helper는 `E*Gnode`의 uniform scale 분리, 월드 m 단위 body offset의 정·역변환만 수행한다. Xbot s=0.0177327·회전 offset·임의 E·30종 회전/배율의 행렬 전체 왕복, 전단·반사·비균일·퇴화·비affine·NaN/inf 거절·1e30/1e-30/denorm_min 양수 uniform 허용과 실패 출력 보존을 확인했다. COM 보정/속도·등록 s 저장·scale 변경 거절·MoveKinematic·Dynamic 보존·section E 동일성/Nmesh 영향은 BC-08a-2 이후 연결 단계에 남아 있다.

산출물·로그는 `$env:TEMP/BreakConstraintTests`의 `SkeletalFrameTestsDebug.exe`, `SkeletalFrameTestsRelease.exe`, `BC08a1-Debug-CPU.log`, `BC08a1-Release-CPU.log`, `BC08a1-Debug-build.log`, `BC08a1-Release-build.log`다. 새 시험은 Jolt/GPU 없이 실행한다. 코드·시험·프로젝트/target 등록은 신규 파일 포함 267줄 추가·1줄 삭제(합계 268줄)다. BC-08a-2 이후 구현, git 쓰기 명령, junction 변경, 기존 .mini·미추적 Report 수동 변경은 하지 않았다.

2026-10-30 새 실행 재검증(BC-08a-1): 변경 분류는 루트 `bc08-split.txt`의 BC-08a/MIXED에 기록했다. 이 단계 파일은 SkeletalPhysicsFrames.h, tests/SkeletalFrameTests.cpp, ModelViewer.vcxproj/.filters의 새 등록, Physics/CMakeLists.txt의 FrameTests target, Tools/BuildJolt.ps1의 testTargets, Docs의 08a-1 설명이다. Report는 범위 밖이므로 기존 위 표의 파일 목록에서 제외했다. CPU Debug 706/706 PASS 2.98초, Release 706/706 PASS 3.12초, 경고0·종료0이다. 전체 빌드와 비물리 실행은 아래 08b의 공통 재검증 결과를 공유한다. 이번 실행에서 수정한 저장소 파일은 이 Progress와 bc08-split.txt뿐이며 코드는 수정하지 않았다.
### BC-08a-2 완료·미커밋 (2026-10-29, `8dc58b08` 기준)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-08a-2 | 미커밋 | MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.h/.cpp, tests/SkeletalBreakTests.cpp; MiniEngine/Physics/PhysicsWorld.h, PhysicsWorldBodies.cpp, PhysicsWorldStep.cpp, CMakeLists.txt; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/Current/CodeMap.md; Docs/README.md | 직접 cl CPU Debug103/103 PASS19.84초·Release103/103 PASS22.59초, 경고0·종료0; 전체 Debug68.01초 PASS·오류0·C4819 20개; Release47.67초 PASS·오류0·C4819 20개·두 구성 종료0; CTest·viewer 미실행(프레임 연결 전) | 독립 읽기 전용 Codex 리뷰의 누적 scale 허용오차 1건 반영, 등록 s 고정 유지·추가 회귀 통과, 최신 source 대조 잔여0건 |

RegisterFromPose는 E와 node pose에서 각 body의 강체 frame·등록 s를 계산한다. Rig는 자산·부모·목표·등록 E 배율을 소유한다. UpdateKinematicTargets는 모든 입력과 body motion·CanMoveKinematic 검증을 마친 뒤 Kinematic만 움직이며 Dynamic pose·속도는 보존한다. 각 목표에는 다음 body 원점 frame, 등록 s, Jolt가 구한 COM 선속도·월드 각속도를 보관한다. 등록 s와 E 배율은 갱신하지 않아 허용오차 누적을 막는다.

새 CPU 검사는 실제 COM offset shape 회전 뒤 원점 목표 도달·COM 속도 차이, 초기 속도0·월드 m offset, 잘못된 마지막 목표·제거된 마지막 body의 선거절과 앞 body 속도 보존, owner scale·극소 dt·누적 scale 변경 거절, Dynamic pose/속도 불변을 확인한다. 코드·시험 변경은 227줄 추가·4줄 삭제(231줄), CMake 등록은 Core include1줄 추가다. 로그와 명령은 `$env:TEMP/BreakConstraintTests/BC08a2-{Debug,Release}-{CPU,build}.log`, `BC08a2-verify.ps1` 및 시험 머리말에 있다. section E 동일성·Nmesh 의존은 Scene 연결 단계, 본 writeback은 BC-08b에서 처리한다.

사용자의 M1 끝까지 Codex 단독 진행 지시를 구현 인계 1절에 반영했다. 기존 BC-08a-1 변경·.mini 3개·Report는 보존했고 커밋·병합·junction 변경은 하지 않았다. 상위는 검증 결과를 확인한 후 다음 새 Codex 실행으로 이어간다.

2026-10-30 새 실행 재검증(BC-08a-2): SkeletalPhysicsSystem.h/.cpp의 Register 메타데이터·RegisterFromPose·UpdateKinematicTargets·GetBodyTarget·Rig 저장 필드, SkeletalBreakTests.cpp의 CheckPoseTargets 및 호출, PhysicsWorld.h의 CanMoveKinematic/GetBodyMotion, PhysicsWorldBodies.cpp/.Step.cpp 구현, Physics/CMakeLists.txt의 Core include와 Docs의 08a-2 기록으로 분류했다. 같은 hunk의 08b 함수와 BC-10 선언은 bc08-split.txt에서 함수/선언 경계로 구분했다. 합쳐진 SkeletalBreakTests CPU Debug 227/227 PASS 32.71초·Release 227/227 PASS 39.25초(08a-2와08b 포함), PhysicsBodyTests 362/362 PASS 31.48/20.84초, PhysicsConstraintTests 286/286 PASS 28.92/27.54초, 모두 컴파일 경고0·종료0이다. 08a-2만 분리한 103건 시험은 다시 만들지 않았고 현재 전체 227건을 재검증했다. 전체 빌드·실행 결과는 아래 공통 기록을 공유한다.
### BC-08b 완료·미커밋 (2026-10-29, `8dc58b08` 기준)

| 단계 | 커밋 | 바뀐 파일 목록 | CPU 테스트·전체 빌드·실행 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-08b | 미커밋 | MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.h/.cpp, tests/SkeletalBreakTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/Current/CodeMap.md; Docs/README.md | 직접 cl CPU Debug227/227 PASS29.38초·Release227/227 PASS28.24초, 경고0·종료0; 전체 Debug67.80초·Release70.70초 PASS·오류0·각 C4819 21개·종료0, /restore 없음; CTest·viewer 미실행(프레임 연결 전) | 독립 읽기 전용 Codex 리뷰 지적0건; git diff --check PASS |

CollectPoseOverrides는 모든 body handle과 운동 방식을 검증하고 Dynamic body만 world 원점 pose에서 등록 s·body offset을 복원한 NodePhysicsPose 배열로 수집한다. E 이동·회전은 허용하되 등록 E 배율은 유지하며 실패하면 기존 출력 배열을 보존한다. Kinematic 본은 기존 애니메이션 경로에 남는다. 기존 EvaluatePoseOverrides·ModelInstance::FinalizePose의 부모 우선 평가와 전체 palette 갱신은 변경하지 않았다.

CPU 시험은 실제 Dynamic 두 body와 4-node graph로 owner 이동·회전 상쇄, 부모 애니메이션 유지, body 없는 tip의 local 애니메이션 상속, Xbot 등록 s 복원, 마지막 stale body·무효 E의 출력 보존을 확인한다. 로그와 재현 명령은 `$env:TEMP/BreakConstraintTests/BC08b-{Debug,Release}-{CPU,build}.log`, `BC08b-verify.ps1` 및 시험 머리말이다. 새 source/target 등록은 없으며 기존 .mini·Report·선행 변경과 junction을 보존하고 커밋·병합하지 않았다. BC-10 새 실행으로 계속 인계한다.

2026-10-30 새 실행 재검증(BC-08b): SkeletalPhysicsSystem.h의 SkeletonPose include·CollectPoseOverrides 선언, .cpp의 CollectPoseOverrides, tests/SkeletalBreakTests.cpp의 CheckPoseMatrix·CheckPoseWriteback·호출 및 Docs의 08b 기록으로 분류했다. 합쳐진 CPU Debug/Release 각각227/227 PASS(32.71/39.25초), 경고0·종료0이다. 이번 실행은 분류를 위한 별도 diff 자기 검토를 했으며 BC-10 혼입(PhysicsWorldBreak.cpp 전체, PhysicsWorld.h와 SkeletalPhysicsSystem.h의 break 관련 선언)을 식별했다. 기존 08a-1 표에 들어간 범위 밖 Report 파일명 1건만 이 문서에서 정정했다. 새 독립 코드 리뷰는 수행하지 않았으며 기존 독립 리뷰 기록은 위에 보존한다.

공통 빌드·실행 재검증: worktree 루트에서 msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows 및 Release, /restore 없이 실행했다. 최초 Debug는 C1041(컴파일러 vc143.pdb 열기 실패) 9개로 15.03초/종료1, Release는 그 최초 스크립트에서 미실행이다. 소스·프로젝트 변경 없이 자식 프로세스 환경 _CL_=/Z7로 재시도하여 Debug 증분8.53초·경고0/오류0, Release109.88초·기존 C4819 경고22개/오류0, 두 구성 종료0으로 통과했다. 이는 clean rebuild가 아니며 BC-10 초안 cpp는 CMake/vcxproj에 미등록이라 이 빌드와 CPU 검사의 검증 대상이 아니다.

비물리 기존 진단: MiniEngine/ModelViewer에서 각 구성 exe에 -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6을 전달하고 Start-Process -WindowStyle Hidden -Wait로 종료를 기다렸다. Debug16.81초·Release 최종10.54초, 모두 종료0·새 MeshComponentChecks.log의 PASS GPU preparation 확인. 최초 Release 진단은 로그가 비어 스크립트 판정 실패였고, 직후 새 명령 생성도 디스크 공간 부족(os error112)으로 실패했다. 이번 실행이 TEMP에 만든 테스트 .obj 12개만 정리한 뒤 Release를 재실행하여 통과했다. 기존 소스·junction·모델·Report는 삭제/수정하지 않았다. .mini 3개와 Report의 SHA-256은 viewer 전후 모두 동일하다. DirectX debug layer 사용, GPU 기반 검증·GPU/driver 재측정·CTest 전체·새 skeletal viewer 진단은 미수행이다. BC-11 Scene 연결과 section E/Nmesh 검증, BC-10 구현은 이번 범위 밖으로 남긴다.

산출물·로그: $env:TEMP/BreakConstraintTests/BC08-recheck-{SkeletalFrameTests,SkeletalBreakTests,PhysicsBodyTests,PhysicsConstraintTests}-{Debug,Release}.log, BC08-recheck-Debug-build.log(최초 실패), BC08-recheck-{Debug,Release}-build-Z7.log(최종 성공), BC08-recheck-{Debug,Release}-component-checks.log(최종 성공), BC08-recheck.ps1 및 build-Z7/viewer-release 재시도 스크립트. git diff --check PASS를 확인하며 git add/commit/stash/merge/push는 하지 않는다. 이번 재검증/분류는 미커밋이다.

### BC-08a·BC-08b 격리 로그 최종 재검증 (2026-10-30, HEAD `8dc58b08`)

공유 TEMP 스크립트·로그가 실행 도중 변경되고 Release 로그 열기가 파일 경합으로 실패하여, 아래 최종 검증은 고유 경로 `$env:TEMP/BreakConstraintTests/BC08-root-1203`에서 따로 수행했다. 앞의 공유 로그 기록은 보존하며 아래 수치는 고유 경로 결과만 사용한다. 소스 수정·git add/commit/stash/merge/push 없이 Progress와 루트 `bc08-split.txt`만 갱신했다.

| 단계 | 변경 파일 분류 | 최종 CPU Debug / Release | 전체 빌드·물리 OFF 진단 |
| --- | --- | --- | --- |
| BC-08a-1 | Scene/SkeletalPhysicsFrames.h, tests/SkeletalFrameTests.cpp; ModelViewer.vcxproj/.filters; Physics/CMakeLists.txt; Tools/BuildJolt.ps1; Docs의 08a 설명 (모든 경로는 bc08-split.txt 참고) | SkeletalFrameTests 706/706 두 구성 PASS, 2.77 / 5.34초 | 아래 공통 최종 결과 공유 |
| BC-08a-2 | Scene/SkeletalPhysicsSystem.h/.cpp의 등록·목표·저장 필드; tests/SkeletalBreakTests.cpp의 CheckPoseTargets; PhysicsWorld.h의 CanMoveKinematic/GetBodyMotion, PhysicsWorldBodies.cpp, PhysicsWorldStep.cpp; CMake Core include; Docs의 08a-2 설명 | 통합 SkeletalBreakTests 227/227 두 구성 PASS, 27.34 / 43.59초 (08b 포함; 08a만의 103건 분리 재구성은 안 함) | 아래 공통 최종 결과 공유 |
| BC-08b | Scene/SkeletalPhysicsSystem.h/.cpp의 SkeletonPose include·CollectPoseOverrides; tests/SkeletalBreakTests.cpp의 CheckPoseMatrix/CheckPoseWriteback 및 호출; Docs의 08b 설명 | 통합 SkeletalBreakTests 227/227 두 구성 PASS, 27.34 / 43.59초 | 아래 공통 최종 결과 공유 |

공통 Physics CPU 최종 결과: PhysicsBodyTests 362건 PASS(31.97 / 13.33초), PhysicsConstraintTests 286/286 PASS(27.99 / 10.26초), PhysicsStepTests 427건 PASS(19.02 / 10.23초). 모든 최종 CPU 컴파일 경고0·종료0이다. 산출물은 고유 경로의 테스트별 하위 폴더, 로그는 `{테스트명}-{Debug,Release}.log`, 측정 결과는 `results.txt`에 있다. 재현 스크립트는 TEMP/BreakConstraintTests/BC08-root-1203.ps1과 BC08-root-1203-retry.ps1이다.

전체 ModelViewer msbuild는 지정 solution·Windows 플랫폼으로 /restore 없이 Debug 9.41초, Release 4.20초에 최종 증분 빌드 PASS했다. 모두 경고0·오류0·종료0이며 clean rebuild는 아니다. 고유 경로의 `build-Debug.log`, `build-Release.log`에 기록했다. 첫 공유 로그 실행의 Debug 빌드도 102.67초·기존 C4819 21개·오류0으로 통과했지만 최종 수치는 격리 재검증을 사용한다.

물리 OFF 기존 진단은 두 구성 모두 `-debug 1 -physics-sim 0`으로 실행했다. `-component-checks 1 -physics-query-exit 6`: Debug 17.33초 / Release 7.48초, `PASS GPU preparation`. `-xbot-arm-checks 1 -physics-query-exit 6`: Debug 31.05초 / Release 9.28초, `BC-03a PASS`·`BC-00 PASS` 모두 확인했다. 네 실행 모두 종료0이며 시작 뒤 새 로그 갱신을 확인했다. Start-Process -WindowStyle Hidden -Wait 사용, 실행 폴더는 MiniEngine/ModelViewer다. 로그 사본은 고유 경로의 `viewer-{Debug,Release}-{MeshComponentChecks,XbotArmChecks}.log`, 측정은 `results-viewer.txt`다. DirectX debug layer 사용, adapter/driver 재측정·CTest 전체·새 skeletal break 화면 진단은 미수행이다.

환경 실패도 남긴다: 공유 로그 경합으로 첫 Release CPU 명령의 로그 열기가 실패했다. 격리 재검증의 첫 Release SkeletalBreakTests 컴파일도 C1085(No space left on device)로 28.58초·종료1이었다. 이번 검증에서 생성한 TEMP 하위 객체 파일만 정리하고 Release 5종과 전체 빌드를 재실행하여 위 최종 결과를 얻었다. 소스·프로젝트 설정·junction은 바꾸지 않았다. `.mini` 3개와 Report의 SHA-256은 모든 viewer 실행 전후 동일하다.

분류용 diff 자기 검토로 BC-10 혼입을 확인했으며 코드 수정과 새 독립 리뷰는 하지 않았다. BC-10은 PhysicsWorldBreak.cpp 전체와 PhysicsWorld.h·SkeletalPhysicsSystem.h의 break 자료형·선언·TopologyGeneration으로 분리 표시했다. 해당 cpp는 현재 CMake/vcxproj 미등록이므로 위 빌드·시험이 BC-10의 성공 증거는 아니다. BC-11 Scene 연결·section E/Nmesh 검증도 범위 밖이다. 커밋은 하지 않았고 BC-08a/08b hunk 분리는 `bc08-split.txt`에 정리했다.
### BC-10 완료·미커밋 (2026-10-30, HEAD 24da5640)

| 단계 | 커밋 | 바뀐 파일 | 검증 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-10a/b | 미커밋(사용자 지시: git add/commit/stash 금지) | MiniEngine/Physics/PhysicsWorldBreak.cpp(초안 완성), CMakeLists.txt, Physics.vcxproj/.filters; MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.cpp, tests/SkeletalBreakTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md, Docs/README.md, Docs/Current/CodeMap.md | CPU Debug403/403 10.02초·Release403/403 13.48초 PASS(컴파일 경고0); 전체 Debug5.19초·Release6.01초 PASS(각 기존 C4819 3개·오류0·/restore없음); 물리 OFF component Debug5.20/Release5.14초·Xbot Debug6.21/Release6.16초 PASS, 모두 종료0 | 자기 리뷰 검증 부족1건 보강, 잔여0건; 독립 codex exec는 home directory 탐색 오류로 시작 불가 |

body3개 유지·관절2→1·root Kinematic/자손 Dynamic·손목 enable/모터 Off·COM 속도 상속과 off-center impulse1회·AlreadyBroken 우선/추가 impulse0·세대 증가·다른 joint stale·이미 Dynamic인 body 보존·broken 경계 제외·해제 후 리소스0을 검사했다. 내부 anchor 0.01m 어긋남과 twist 제한 초과는 UnsupportedPose, 외부 pin은 UnsupportedTopology로 변경 없이 거절했다. 자기 리뷰에서 실제 손목 유지/외부 pin 검증 부족1건을 보강하여 120step 후 anchor≤0.01m를 확인했다. 최초 CPU278/278(11.53/16.21초)·전체 빌드13.35/9.68초도 통과했으며 위 표는 보강 후 최종값이다. 추가 diff 자기 리뷰에서 함수·단락 사이 빈 줄과 기존 BOM 보존을 정리했다. BC-10a(world API/등록 약205줄)·10b(rig/시험 약195줄)로 §11에 행을 추가했으며 의미상 계약 차이는 없다.

직접 cl은 시험 머리말 명령에 PhysicsWorldBreak.cpp를 추가했고 Release는 /MD /O2 /DNDEBUG 및 Release/Jolt.lib를 사용했다. 기존 SkeletalBreakTests CMake target/BuildJolt.ps1 testTargets를 재사용했다. 전체 빌드는 worktree 루트의 msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows 및 Release다. 증분 빌드이며 clean rebuild·CTest 전체는 미수행이다.

뷰어는 MiniEngine/ModelViewer에서 ../Build/x64/{Debug,Release}/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6과 -xbot-arm-checks 1로 각각 실행했다. Start-Process -WindowStyle Hidden -Wait로 종료를 기다려 새 로그의 PASS GPU preparation, BC-00 PASS·BC-03a PASS를 확인했다. 첫 Debug component 판정 스크립트가 정상 PASS 항목의 Failed placement 문자열을 실패로 오인했다(프로세스 종료0·실제 PASS). 실패 행 시작만 검사하도록 TEMP 스크립트를 고치고 네 실행을 재검증했다. DirectX debug layer 사용, GPU/driver 재측정·GPU 기반 검증·BC-11 이후 화면 연결은 미수행이다.

로그/산출물은 $env:TEMP/BreakConstraintTests/BC10-root의 SkeletalBreakTests-{Debug,Release}.log, build-{Debug,Release}.log, viewer-*.log, results.txt, results-viewer.txt, verify.ps1, viewer.ps1이다. 독립 리뷰는 codex.ps1 실행정책 차단 후 codex.cmd로 재시도했으나 Error finding codex home: Could not find home directory로 시작되지 않았다. 별도 읽기 전용 diff/초안/계약 자기 리뷰를 수행했다. git 쓰기·메인 체크아웃 수동 수정·junction 변경·.mini/Report 수동 변경 없음이며 viewer 전후 보호 파일 SHA-256 동일이다. BC-11 이후는 착수하지 않았다.

2026-10-30 BC-10 독립 리뷰 지적 1건 처리: 기존 각도 제한 시험의 anchor 선거절을 손 위치 보정으로 제거하고 실제 COM anchor 일치·순수 +X twist/+Y swing·각각 초과 거절 후 상태 불변·각각 제한 안 성공을 검사했다. CPU Debug482/482(15.00초)·Release482/482(27.80초) PASS, 경고0·종료0이며 기존403건 대비79건 증가다. TEMP의 twist/swing 검사 개별 우회 변형도 각각 해당 초과 거절 검사에서 실패해 분기 검증을 확인했다. 구현 버그 없음·구현 코드 변경 없음으로 전체 빌드는 재실행하지 않았다. 로그/변형 소스는 $env:TEMP/BreakConstraintTests/BC10-review-fix, BC-11 미착수다.
