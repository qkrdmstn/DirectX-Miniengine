상태: 진행 중 — BC-04a `8593a0be`, BC-04b `d31863a7`, BC-05 `84ecea1f`, BC-06 `f6a14195`, BC-07 구현·검증 완료(미커밋), BC-08a 이후 미착수 (2026-10-29)

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
