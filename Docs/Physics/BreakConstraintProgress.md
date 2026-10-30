상태: 진행 중 — BC-12a-2 b4afd069 완료; BC-12b 렌더 팔레트 수치 보완 완료(미커밋), 두 구성 각2회 PASS·화면 확인/캡처는 Claude 후속; BC-13a 미착수 (2026-10-30)

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
| BC-10a/b | `e7a608dc` | MiniEngine/Physics/PhysicsWorldBreak.cpp(초안 완성), CMakeLists.txt, Physics.vcxproj/.filters; MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.cpp, tests/SkeletalBreakTests.cpp; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md, Docs/README.md, Docs/Current/CodeMap.md | CPU Debug403/403 10.02초·Release403/403 13.48초 PASS(컴파일 경고0); 전체 Debug5.19초·Release6.01초 PASS(각 기존 C4819 3개·오류0·/restore없음); 물리 OFF component Debug5.20/Release5.14초·Xbot Debug6.21/Release6.16초 PASS, 모두 종료0 | 자기 리뷰 검증 부족1건 보강, 잔여0건; 독립 codex exec는 home directory 탐색 오류로 시작 불가 |

body3개 유지·관절2→1·root Kinematic/자손 Dynamic·손목 enable/모터 Off·COM 속도 상속과 off-center impulse1회·AlreadyBroken 우선/추가 impulse0·세대 증가·다른 joint stale·이미 Dynamic인 body 보존·broken 경계 제외·해제 후 리소스0을 검사했다. 내부 anchor 0.01m 어긋남과 twist 제한 초과는 UnsupportedPose, 외부 pin은 UnsupportedTopology로 변경 없이 거절했다. 자기 리뷰에서 실제 손목 유지/외부 pin 검증 부족1건을 보강하여 120step 후 anchor≤0.01m를 확인했다. 최초 CPU278/278(11.53/16.21초)·전체 빌드13.35/9.68초도 통과했으며 위 표는 보강 후 최종값이다. 추가 diff 자기 리뷰에서 함수·단락 사이 빈 줄과 기존 BOM 보존을 정리했다. BC-10a(world API/등록 약205줄)·10b(rig/시험 약195줄)로 §11에 행을 추가했으며 의미상 계약 차이는 없다.

직접 cl은 시험 머리말 명령에 PhysicsWorldBreak.cpp를 추가했고 Release는 /MD /O2 /DNDEBUG 및 Release/Jolt.lib를 사용했다. 기존 SkeletalBreakTests CMake target/BuildJolt.ps1 testTargets를 재사용했다. 전체 빌드는 worktree 루트의 msbuild MiniEngine/ModelViewer/ModelViewer.slnx /m /p:Configuration=Debug /p:Platform=Windows 및 Release다. 증분 빌드이며 clean rebuild·CTest 전체는 미수행이다.

뷰어는 MiniEngine/ModelViewer에서 ../Build/x64/{Debug,Release}/Output/ModelViewer/ModelViewer.exe -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6과 -xbot-arm-checks 1로 각각 실행했다. Start-Process -WindowStyle Hidden -Wait로 종료를 기다려 새 로그의 PASS GPU preparation, BC-00 PASS·BC-03a PASS를 확인했다. 첫 Debug component 판정 스크립트가 정상 PASS 항목의 Failed placement 문자열을 실패로 오인했다(프로세스 종료0·실제 PASS). 실패 행 시작만 검사하도록 TEMP 스크립트를 고치고 네 실행을 재검증했다. DirectX debug layer 사용, GPU/driver 재측정·GPU 기반 검증·BC-11 이후 화면 연결은 미수행이다.

로그/산출물은 $env:TEMP/BreakConstraintTests/BC10-root의 SkeletalBreakTests-{Debug,Release}.log, build-{Debug,Release}.log, viewer-*.log, results.txt, results-viewer.txt, verify.ps1, viewer.ps1이다. 독립 리뷰는 codex.ps1 실행정책 차단 후 codex.cmd로 재시도했으나 Error finding codex home: Could not find home directory로 시작되지 않았다. 별도 읽기 전용 diff/초안/계약 자기 리뷰를 수행했다. git 쓰기·메인 체크아웃 수동 수정·junction 변경·.mini/Report 수동 변경 없음이며 viewer 전후 보호 파일 SHA-256 동일이다. BC-11 이후는 착수하지 않았다.

2026-10-30 BC-10 독립 리뷰 지적 1건 처리: 기존 각도 제한 시험의 anchor 선거절을 손 위치 보정으로 제거하고 실제 COM anchor 일치·순수 +X twist/+Y swing·각각 초과 거절 후 상태 불변·각각 제한 안 성공을 검사했다. CPU Debug482/482(15.00초)·Release482/482(27.80초) PASS, 경고0·종료0이며 기존403건 대비79건 증가다. TEMP의 twist/swing 검사 개별 우회 변형도 각각 해당 초과 거절 검사에서 실패해 분기 검증을 확인했다. 구현 버그 없음·구현 코드 변경 없음으로 전체 빌드는 재실행하지 않았다. 로그/변형 소스는 $env:TEMP/BreakConstraintTests/BC10-review-fix, BC-11 미착수다.

### BC-11 완료·미커밋 (2026-10-30, HEAD `e7a608dc`)

| 단계 | 커밋 | 바뀐 파일 | 검증 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-11 | 미커밋(Claude 커밋 담당) | MiniEngine/ModelViewer/ModelViewer.cpp; Scene/PhysicsSimulation.cpp, PhysicsSystem.h/.cpp, SkeletalPhysicsSystem.h, Scene.h/.cpp, tests/MeshComponentChecks.h; Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md, Docs/README.md, Docs/Current/CodeMap.md | CPU Debug/Release 각2790/2790(7종) PASS; 전체 리뷰후Debug54.24초·Release68.79초 PASS(오류0·C4819경고19/19·/restore없음); Debug 물리OFF component15.07초·Xbot15.68초 PASS·종료0, BC-11 프레임/실제rig/animation 44검사 PASS; 기본6.24초 종료0·Static 442검사 PASS | 독립 리뷰3건 수정; break 선소비·target 생략 변형은 새 예상 목표 검사에서 실패 확인; 아래 재검증 기록 참조 |

BC-10 커밋 칸을 e7a608dc로 채웠다. 공통 PhysicsStepClock의1/60초·최대4회 정책 안에서 animation→Kinematic target→대기break→world step1회→Static/본writeback→owner별pose확정으로 연결했다. GPU 준비는 렌더 프레임당1회이며 비물리는 기존 Update를 유지한다. zero-step/pause에는 대기break와 이전pose를 유지하고 신규모델만 dt=0으로 준비한다. 실제section E 일치·Nmesh ancestry·uniform frame을 검증하며 world 공개는 없다. 의미상 계약 차이 없음, BC-12a 이후 미착수, git add/commit/stash/merge/push 없음.

CPU 결과(Debug/Release, 컴파일경고0): SkeletalBreakTests482/482(19.44/22.73초), PhysicsBodyTests362(14.70/16.91초), PhysicsConstraintTests286/286(20.71/21.53초), PhysicsStepTests427(24.83/12.78초), SkeletalFrameTests706/706(2.18/1.38초), SceneMathTests495, SkeletonPoseTests32. SceneMath/SkeletonPose 시간은 별도 측정하지 않았다. 산출물/로그/재현스크립트는 $env:TEMP/BreakConstraintTests/BC11-root의 verify.ps1, pose-tests.ps1, build-final.ps1, viewer.ps1, viewer-extra.ps1 및 이름별 로그다. 최초 전체빌드67.70/46.93초도 성공했다. 신규 C4244는 NodeId 명시변환으로 수정 후 최종빌드 통과했다. clean rebuild/CTest 전체/Release뷰어 미수행이다.

MiniEngine/ModelViewer에서 Debug exe -debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6 및 -xbot-arm-checks 1을 Start-Process -WindowStyle Hidden -Wait로 실행했다. 새 MeshComponentChecks.log의 BC-11 PASS39개·PASS GPU preparation, XbotArmChecks.log의 BC-00/03a PASS를 확인했다. 별도복제Scene에서 step0/1/4/0, wrapper와 전체node/position/normal팔레트동등, GPU0/substep·1/frame, 실제정규화Xbot queued break→Dynamic2개 writeback, 이후zero-step보존·자원0을 확인했다. 기본실행은 검사옵션 없이 물리OFF와 종료옵션만 사용해 종료0, 기존Static진단은 -physics-sim 1 -physics-sim-checks 1 -physics-query-exit 6으로 종료0이나 PASS문자열은 별도캡처하지 못했다. DirectX debug layer 사용, GPU기반검증·GPU/driver재측정·BC-12 시각데모 미수행이다. .mini3개와 Report SHA-256은 모든 실행 전후 동일하며 메인체크아웃·junction은 수동수정하지 않았다.

#### BC-11 독립 리뷰 지적 3건 수정 (2026-10-30, `e7a608dc` 기준 미커밋)

1. [중간] GPU 준비 검사의 자기충족성: `PhysicsSystem::UpdateFrame`으로 simulation 진행과 GPU 준비를 묶었다. `ModelViewer::Update`와 `CheckFixedSimulationPoses`가 같은 함수를 호출하고, 준비 카운터를 그 전후로 비교한다. substep 0/1/4/0의 모든 프레임에서 +1을 확인한다. 검사 자체의 직접 `PrepareMeshes` 호출과 중복 카운터 검사를 제거했다.
2. [중간] target→break→step 순서의 증거 부족: `CheckAnimatedBreakOrder`에 root가 X로 6m/s 움직이는 두 body·한 관절 fixture를 추가했다. 두 animation key의 기울기로 독립 계산한 이번 substep 목표 x=6dt, COM 선속도 (6,0,0)·각속도0, 분리 후 x=6dt+6dt와 y=1.5−9.8dt²를 검사한다. X 이동에는 기본 damping의 작은 오차만 허용하며 world 값을 서로 비교하지 않는다. 기존 Xbot 검사도 유지했다. 기존 39건에서 중복 프레임 검사4건을 줄이고 fixture9건을 더해 **BC-11 총44건 PASS**다.
3. [낮음] `AdvanceSimulation`의 substep 본문을 animation 평가·Kinematic 목표·break 소비·world step·writeback/확정의 다섯 기능 단락으로 나누고, 빈 줄과 단락별 한 줄 한글 주석을 붙였다. 추가 함수의 역할 주석도 기록했다.

재검증: CPU 7종은 머리말의 cl 명령·Jolt 설정으로 Debug/Release 각각2790/2790 PASS(482 break, 362 body, 286 constraint, 427 step, 706 frame, 495 scene math, 32 skeleton pose). worktree 루트 `.slnx /m /p:Configuration={Debug,Release} /p:Platform=Windows` 빌드는 /restore 없이 Debug54.24초·Release68.79초, 각각 오류0·C4819 경고19건이다. Debug `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6`에서 BC-11 44건·최종 GPU preparation PASS(15.07초), 기존 Xbot 진단 BC-00/03a PASS(15.68초), 물리OFF 기본 실행 종료0(6.24초)를 확인했다. 기존 `-physics-sim 1 -physics-sim-checks 1`은 디버그 출력 수집 실행에서 validation passed·442검사·종료0(55.98초)을 확인했다. DirectX debug layer 사용, GPU 기반 검증은 사용하지 않았다. Static 진단 종료 시 기존 Light 관련 live-resource 경고가 있었고 D3D12 ERROR/CORRUPTION은 없었다.

순서 변형 검증: `PhysicsSimulation.cpp` 원본 바이트를 TEMP에 보관한 뒤 break 소비를 target 갱신 앞으로 옮긴 Debug 변형과 target 갱신을 생략한 Debug 변형을 각각 빌드·실행했다. 둘 다 `FAIL ... BC-11 target equals independent animation sample`을 기록했다. 실패 예외 후 프로세스가 남아 45초 제한으로 해당 진단 프로세스를 종료했다. 각 실행의 finally에서 원본 바이트를 복구했고 SHA-256 동일을 확인했다. 구현 동작 버그는 발견하지 않았으며 검증 경로·회귀 증거·주석만 보완했다. BC-12a는 시작하지 않았다.

로그·CPU 산출물·변형 전 소스는 `$env:TEMP/BreakConstraintTests/BC11-review-fix`에 있다. `verify.ps1`, `pose-tests.ps1`, `viewer.ps1`, `capture-viewer.ps1`, `mutations.ps1`과 검사별 로그를 보관한다.

변형 복구 후 최종 확인: 원본 바이트 SHA-256 동일, Debug 재빌드9.17초·오류0·C4819 경고3건, component 재실행28.97초·종료0·PASS 행703개(BC-11 44건 포함)·FAIL0·최종 GPU preparation PASS를 확인했다. 디버그 출력의 D3D12 ERROR/CORRUPTION은0이며 종료 시 Light 관련 live-resource 경고는 위 Static 실행과 동일하다. 실행 전후 .mini 3개와 Report의 SHA-256이 모두 같고 git add/commit/stash 및 메인 체크아웃 수동 수정은 하지 않았다. git diff --check PASS다.

### BC-12a-1 진단 조회·독립 impulse 시험 (2026-10-30, HEAD `686d1c7a`)

- 분할: BC-12a를 12a-1(실제 body/관절 진단 조회 API·CPU 시험)과 12a-2(뷰어 옵션·overlay·자동 판정)로 나눴다. 이유는 계약 문서 §12 BC-12a 분할 문단.
- 바뀐 파일: `SkeletalPhysicsSystem.h/.cpp`(`ReadDiagnosticState`, 상태 구조체 3개), `Scene/tests/SkeletalBreakTests.cpp`(`CheckDiagnosticState`), `Physics/tests/PhysicsBodyTests.cpp`(`TestIsolatedOffCenterImpulse`). 코드 약 190줄.
- CPU: SkeletalBreakTests·PhysicsBodyTests Debug/Release 모두 종료0 PASS(36.02/39.36초, 21.33/26.56초).
- 빌드: Debug 57.05초 오류0(Codex), Release 150초 오류0(Claude 재실행). 기존 C4819 경고만 있음.
- 실행: Debug `-debug 1 -physics-sim 0 -component-checks 1 -physics-query-exit 6` 종료0·PASS 703행·실패0·`PASS GPU preparation`, 물리 OFF 기본 실행 종료0. 이번 단계는 뷰어 호출 경로가 없어 화면 변화 없음.
- 리뷰: Codex 사용량 한도로 구현 실행이 Release 빌드 기록 중 끊겼다. Claude가 이어받아 diff 자기 리뷰(지적 0: 함수·단락 한글 주석, 실패 시 출력 보존, impulse 기대값 r×J/I=6 손계산 일치)와 Release 빌드·뷰어 실행을 했다.
- 로그: `$env:TEMP/BreakConstraintTests/BC12a-root`, `$env:TEMP/BreakConstraintTests/BC12a-claude`.

### BC-12a-2 fixture 뷰어 진단 (2026-10-30, HEAD dd219b8e)

| 단계 | 커밋 | 바뀐 파일 | 테스트·빌드·실행 결과와 측정 시간 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-12a-2 | 미커밋 | ModelViewer.cpp; ModelViewer.vcxproj/.filters; Scene/tests/SkeletalPhysicsChecks.h(신규); Docs/Physics/BreakConstraintImplementation.md, BreakConstraintProgress.md; Docs/README.md | CPU SkeletalBreak Debug/Release909/909 PASS(13.11/24.80초), PhysicsBody393 PASS(11.72/15.32초); 전체 Debug17.76초/Release167.11초 PASS·오류0·기존 C4819 4/33개; Debug fixture41.81초 BC-12a PASS·종료0; 물리OFF 기본41.03초·component30.54초 종료0·GPU preparation PASS; 모든 viewer D3D12 ERROR/CORRUPTION0 | 독립 codex exec는 home directory 탐색 실패; 별도 읽기 전용 자기 리뷰 지적1건(.filters 잘못된 Scene 필터) 수정, 잔여0 |

- 별도 4-node 모델 하나를 기존 모델 정규화 뒤 추가한다. 기존 모델/메시/weight 변경 없이 공통 UpdateFrame으로 프레임당1 fixed tick. tick60 완료 뒤 joint0 zero impulse queue→tick61 target 뒤 break→tick180 판정. 본 청록·COM 노랑·양쪽 anchor 빨강/초록 overlay, 기존 근평면 clipping/projection 재사용.
- 수치: rootDeltaY=0m, childDeltaY=-2.51435m, wristMaxError=0.00992500037m(≤0.01m), finite=1, joint0 broken/손목 enabled·motorsOff 유지. 종료0은 PASS 판정을 대신하지 않으며 파일/debugger의 BC-12a PASS를 확인했다.
- viewer 명령: ModelViewer 폴더에서 Debug exe에 `-debug 1 -skeletal-break-checks 1`; 회귀는 `-debug 1 -physics-sim 0 -physics-query-exit 6` 및 `-component-checks 1` 추가. debugger 수집기는 자식 프로세스180초 제한, 회귀 실행은 Start-Process -WindowStyle Hidden -Wait로 수집기를 기다렸다. GPU NVIDIA GeForce MX450(1906MB), driver는 레지스트리에서 32.0.15.9186 확인(CIM 조회는 권한 거절). GPU 기반 검증 미사용, 기존 Light live-resource 경고가 남지만 D3D12 ERROR/CORRUPTION0.
- 스크린샷: 기존 capture가 overlay 전에 scene color만 저장하므로 overlay 포함 분리 전/후 스크린샷 기능은 없어 미확보. 사람이 청록 본 연결·COM 낙하·손목 anchor 유지와 원래 모델 배치를 화면에서 확인해야 한다.
- 최초 Debug 컴파일은 Matrix4 위치 조회/Color 정수 overload 오류로11.02초 실패. GetW·float Color로 고친 뒤 위 최종 결과를 얻었다. Python 실행은 환경의 접근 거절로 시작하지 못해 PowerShell/.NET으로 편집했다. 빌드·시험·뷰어는 차단되지 않았다.
- 로그/재현: `$env:TEMP/BreakConstraintTests/BC12a2-root`의 verify.ps1, capture-viewer.ps1, self-review.txt, results.txt/results-extra.txt, build-*.log, viewer-*-debug.log, fixture-Debug.log, component-Debug.log. .mini3개·Report 전후 SHA-256 동일. git add/commit/stash/merge/push, 메인 체크아웃 수동 수정, junction 변경 없음. CTest 전체/Release viewer 미수행. BC-12b 이후 미착수.
- 최종 fixture 재실행: Start-Process -WindowStyle Hidden -Wait, 수집기 내부180초 제한, 28.90초·동일 수치 BC-12a PASS·종료0·D3D12 ERROR/CORRUPTION0. fixture-Debug-final.log와 run-skeletal-final.txt에 기록. 최종 보호 파일 SHA-256 동일·git diff --check PASS.

#### BC-12a-2 독립 리뷰 지적 수정·재검증 (2026-10-30, HEAD dd219b8e, 미커밋)

- **독립 리뷰(Codex) 지적 3건 수정**: ① 외부 화면 캡처 시도 후 미확보를 명시하고 계약 상태/§11/README를 “구현 완료·화면 검증 대기”로 정정 ② 이 문서 첫 상태 줄의 선행 완료/현재 검증 상태 정정 ③ Startup fixture/RenderUI overlay 앞 빈 줄, Draw mark 역할·본/COM/anchor별 한글 주석과 단락 정리. 추가 별도 읽기 전용 자기 리뷰에서 동작 변경 없음·요청 주석/공백 확인, 잔여 코드 지적0건. 새 독립 리뷰 실행은 하지 않았다.
- 이번 변경 파일은 ModelViewer.cpp(빈 줄2개), Scene/tests/SkeletalPhysicsChecks.h(주석·단락만 +9/-1행), BreakConstraintImplementation.md(상태/§11/§12), 이 Progress, Docs/README.md의 5개다. 기존 vcxproj/.filters 변경은 그대로 보존했다. 새 기능·기준 완화·판정/종료 타이밍 변경 없음.
- CPU 머리말 직접 cl 명령 재검증: SkeletalBreakTests Debug909/909 12.63초·Release909/909 18.94초, PhysicsBodyTests Debug393건12.72초·Release393건22.67초, 모두 PASS·종료0·컴파일 경고0. TEMP/BreakConstraintTests/BC12a2-root의 기존 산출물 폴더를 재사용하고 이번 생성 obj만 삭제하여 큰 산출물 복사본을 만들지 않았다.
- 전체 빌드: worktree 루트의 지정 .slnx /m /p:Configuration={Debug,Release} /p:Platform=Windows, /restore 없음. 최종 Debug27.77초/Release22.21초 PASS·종료0·각 오류0·기존 C4819 4건. 최초25.75/16.95초도 성공했으나 편집 중 빠진 ModelViewer.cpp 기존 BOM을 복구하고 mark 앞 빈 줄을 보강해 최종 재빌드했다.
- fixture 실행: Debug -debug 1 -skeletal-break-checks 1, 34.19초·BC-12a PASS·종료0, rootDeltaY=0·childDeltaY=-2.51435m·손목 최대0.00992500037m(0.01m 대비 여유0.75%). **Release 동일 옵션36.51초·재실행32.76초 모두 BC-12a FAIL·종료0**, rootDeltaY=0·childDeltaY=-2.53709m·손목 최대0.0133718159m(기준보다33.72% 큼). 두 구성 모두180 ticks·finite/topology=1. Release는 PASS가 아니며 오차 수정은 미해결·이번 지적 수정 범위 밖이다.
- Debug 물리 OFF -debug 1 -physics-sim 0 -physics-query-exit 6 기본35.47초·종료0, 여기에 -component-checks 1 추가35.17초·종료0·PASS703행/FAIL0·PASS GPU preparation. 최종5회 viewer 모두 D3D12 ERROR/CORRUPTION0, 기존 Light live-resource 경고 유지. Start-Process -WindowStyle Hidden -Wait와 내부 viewer180초 제한 사용. GPU/driver는 이전 동일 환경(NVIDIA MX450/32.0.15.9186), 이번 재조회·GPU 기반 검증·CTest 전체 미수행.
- 캡처: PowerShell/System.Drawing CopyFromScreen으로 창 발견(28.122초) 약0.5초 뒤부터 종료 직전(마지막33.903초)까지 반복 시도했으나 “핸들이 잘못되었습니다”로 전부 실패. TEMP/BreakConstraintTests/BC12a2-screens에 PNG 없음·직접 이미지 확인 불가이므로 본 청록/COM 노랑/anchor 빨강·초록과 분리 전후 화면은 사람이 확인해야 한다. 초기 수집기의 동기 ShowWindow 대기는 이번 실행의 수집기/자식만 종료하고 비동기 ShowWindowAsync로 수정하여 재실행했다. 해당 중단 실행은 PASS 근거에서 제외했다.
- 로그/재현: TEMP/BreakConstraintTests/BC12a2-review-fix의 verify.ps1, build-final.ps1, capture-viewer.ps1, viewers.ps1, build-*-final.log, fixture-{Debug,Release,Release-repeat}.log, viewer-*-debug.log, component-Debug.log. 보호 대상 .mini3개·Report SHA-256 전후 동일. git add/commit/stash/merge/push·메인 체크아웃 수정·junction 변경 없음, BC-12b 미착수.

#### BC-12a-2 Release 손목 오차 원인 확인·판정 정리 (2026-10-30, HEAD dd219b8e, 미커밋)

| 단계 | 커밋 | 바뀐 파일 | 검증·리뷰 |
| --- | --- | --- | --- |
| BC-12a-2 후속 | 미커밋 | Scene/tests/SkeletalPhysicsChecks.h(+40/-4); BreakConstraintImplementation.md(상태·§11·§12); BreakConstraintProgress.md; Docs/README.md | 관련 CPU 두 구성 PASS·전체 Debug/Release 빌드 PASS·fixture 각2회 PASS·Debug 물리OFF/component PASS; 별도 자기 리뷰 지적0건 |
- 판정 변경 전 각1회: Debug34.25초 PASS, Release28.25초 FAIL(기존 전체구간≤0.01m). 두 구성 tick98에서 바닥 충돌(상면-1.155179m, 하위 COM Y≈-0.8627m, Vy=-5.9486→+1.822m/s). 최대는 Debug tick131=0.00992500037m/하위COM Y=-0.88278234m, Release tick139=0.0133718159m/하위COM Y=-0.886882186m. 둘 다 접촉 이후여서 사용자 전달 Claude 판정대로 자유낙하 최대≤0.01m·접촉 이후 최대≤0.02m·마지막≤0.01m로 변경. 충돌 solver 순간 오차와 Release 최적화(/fp:fast 포함)에 따른 차이로 해석하며 옵션 단독 원인은 미분리다.
- 접촉은 하위 COM이 실제 바닥상면+0.30m(반높이0.25+여유0.05) 이하이고 Vy 증가가1m/s/tick 초과인 최초 tick으로 고정. tick별 오차·COM·속도, 접촉/최대 tick, 구간 최대·마지막 오차만 기록·판정한다. 물리 구현·형상·GPU·메시·웨이트·기존 옵션 동작 변경 없음.
- CPU 머리말 cl 명령/Jolt 설정: SkeletalBreak Debug909/909(25.13초)·Release909/909(32.79초), PhysicsBody Debug393(20.06초)·Release393(29.67초), 모두 PASS·종료0·컴파일경고0. TEMP 기존 CPU 출력 폴더를 재사용하고 이번 obj만 정리했다. 전체 msbuild Debug17.19초·Release31.38초, /restore 없이 PASS·오류0·기존 C4819 각4건. 측정 로그만 넣은 최초 빌드42.97/48.30초도 PASS.
- 최종 fixture `-debug 1 -skeletal-break-checks 1`: Debug52.55/31.69초·Release36.21/24.72초 각2회 PASS·종료0. 접촉tick98·접촉전최대2.384185791e-7m 공통, Debug 접촉후최대0.00992500037m/마지막0.009835194796m/rootDelta0/childDelta-2.514346242m, Release 접촉후최대0.0133718159m/마지막0.0001645183365m/rootDelta0/childDelta-2.537089825m. 각 구성 반복 수치 동일·finite/topology=1.
- Debug 물리OFF 기본 `-debug 1 -physics-sim 0 -physics-query-exit 6` 39.63초·종료0, 같은 명령에 `-component-checks 1` 추가39.16초·종료0·703 PASS행/FAIL0·PASS GPU preparation. 최종6회 모두 D3D12 ERROR/CORRUPTION0(기존 경고는 유지), GPU/driver는 이전 기록 NVIDIA MX450/32.0.15.9186, 이번 재조회·GPU 기반검증·CTest 전체는 미수행. Start-Process -WindowStyle Hidden -Wait, 내부 viewer180초 제한.
- 화면: **화면 캡처 확인(Claude), 사람 확인 대기 없음**. 사용자 전달대로 샌드박스 밖 Claude 캡처 성공을 반영: `$env:TEMP/BreakConstraintTests/BC12a2-screens/t002.7.png`(분리 전 세로 본 사슬·COM·anchor), `t005.4.png`(분리 후 아래 두 body가 바닥에 누움). 파일 존재 확인, 재캡처·추가 화면 확인 요청 없음. 이전 캡처 실패/화면 대기 기록은 당시 이력이다.
- 리뷰: `codex.cmd exec -s read-only --ephemeral`는 `Error finding codex home: Could not find home directory`로 시작 실패. 별도 읽기 전용 diff/계약 자기 리뷰로 대체(지적0건): 접촉tick을 오차와 독립 판단·접촉tick부터 이후구간·최종오차 별도 제한·무접촉 FAIL·물리OFF 경로 불변·한글 역할/단락 주석·코드44줄 변경 확인. 보호대상 .mini3개/Report SHA-256 전후 동일. git add/commit/stash/merge/push·메인 체크아웃 수동 수정·junction 변경 없음, BC-12b 미착수.
- 로그·재현: `$env:TEMP/BreakConstraintTests/BC12a2-contact`의 measure-{Debug,Release}.log, final-{Debug,Release}-skeletal-{1,2}.log, final-Debug-{basic,component}-1-debug.log, results*.txt, verify.ps1, build.ps1, final-viewers.ps1, collect.ps1(캡처 호출 제거), self-review.txt. 미해결 사항 없음(최종 검증 기준). 커밋은 바깥 Claude 담당.

### BC-12b Xbot 왼팔 분리 진단 (2026-10-30, HEAD b4afd069)

| 단계 | 커밋 | 바뀐 파일 | 검증·시간 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-12b | 미커밋 | ModelViewer.cpp(+5/-1); Scene/tests/SkeletalPhysicsChecks.h(+137/-19); BreakConstraintImplementation.md; BreakConstraintProgress.md; Docs/README.md | CPU 두 구성 PASS, 전체 Debug/Release PASS, Xbot 각2회 PASS, 물리OFF/기존 component PASS, 기존 fixture 두 구성 PASS; 상세 아래 | 독립 codex home 오류로 시작 불가 → 별도 자기 리뷰 지적1(기능 단락 주석) 수정, 잔여0 |

- 구현: 기존 시작 Xbot에 BC-04b asset의 body3/joint2 등록, animation0 재생. SkeletalPhysicsChecks의 기존 60/180 tick·진단 조회·overlay·구간별 손목 판정 재사용. 상완은 별도 ModelInstance의 animation model pose 16성분과 매 tick 비교(≤1e-5). 전완 하강 기준은 분리 직전 tick60 COM. 월드 +X impulse0.05kg·m/s, 직전 전완 COM에 1회 요청. unbroken2→1·enabled0→1·Kinematic1/Dynamic2·motorsOff·전체 본 pose 및 body 속도 finite 검사. 메시/웨이트/정점 변경 없음.
- CPU: 파일 머리말 cl 명령/Jolt 설정, TEMP의 이전 BC12a2-root CPU 출력 폴더를 재사용(새 큰 복제 없음). SkeletalBreak Debug909/909 20.37초·Release909/909 29.33초, PhysicsBody Debug393건14.37초·Release393건28.35초, 모두 종료0·컴파일 경고0. 이번 생성 obj만 제거. CTest 전체 미수행.
- 전체 빌드: worktree 루트 `.slnx /m /p:Configuration={Debug,Release} /p:Platform=Windows`, /restore 없음. 최초 Debug20.84초·Release30.55초 PASS. 주석 정리 후 뷰어가 실행 중인 상태에서 Debug 링크를 시작해 LNK1168(exe 잠금) 21.76초 실패; 뷰어 정상 종료를 확인하고 재빌드한 최종 Debug11.18초·Release25.53초 PASS, 오류0·기존 C4819 각4건. 기능 코드는 최초 빌드와 동일하며 최종 빌드는 주석 정리만 반영.
- Xbot `-debug 1 -skeletal-break-xbot 1`: Debug49.39/35.04초, Release25.85/35.40초 각2회 BC-12b PASS·종료0. 네 실행 모두180ticks·finite/topology=1·unbroken2→1·enabled0→1·상완 animationMaxError0. Debug 반복 수치 동일: 전완deltaY=-1.36082375m, 손목 접촉전 최대0.000006665660294m/접촉후 최대0.006326892879m/마지막0.00002529360427m. Release 반복 동일: deltaY=-1.360850334m, 손목 접촉전0.000006643729193m/접촉후0.006328773685m/마지막0.00004452272333m. 네 실행 모두 contact/max tick88. 손목 허용값은 기존 0.01/0.02/0.01m 유지; BC-00 break 사전 허용값과 solver 오차의 구분을 계약 §12에 명확히 기록.
- 회귀: Debug `-debug 1 -physics-sim 0 -physics-query-exit 6` 기본35.13초 정상종료0, 같은 옵션에 `-component-checks 1` 추가26.83초703 PASS·FAIL0·PASS GPU preparation·종료0. 공통 검사 코드를 수정했으므로 기존 `-skeletal-break-checks 1`도 최종 바이너리에서 Debug41.20초·Release46.54초 각각 BC-12a PASS·종료0. fixture 손목 수치는 기존 기록과 동일(Debug 최대0.00992500037m, Release 최대0.0133718159m; 구간별 판정 PASS).
- 뷰어8회 모두 D3D12 ERROR/CORRUPTION0(디버거 출력 수집). 기존 종료 Light live-resource 경고는 유지. DirectX debug layer 사용, GPU 기반 검증 미사용. 환경은 이전 기록 NVIDIA GeForce MX450/driver32.0.15.9186이며 이번 재조회 없음. Start-Process -WindowStyle Hidden -Wait로 수집기를 실행, 내부 viewer180초 제한. 화면 캡처 코드는 호출하지 않았고 캡처 시도 없음. 기본 VSync와 60/120 tick 타이밍 유지. **화면 확인·분리 전후 캡처는 Claude 후속**: 상완/몸통 애니메이션, 전완·손 동반 낙하, 손목 연결 유지, 팔꿈치 피부 늘어짐, 본/COM/anchor overlay를 사람이 확인해야 한다. 수치 기준 미해결 없음, 시각 검증 미완료.
- 재현/로그: `$env:TEMP/BreakConstraintTests/BC12b`의 verify.ps1, build-final.ps1, collect.ps1(캡처 호출 없음), viewers.ps1, fixtures.ps1, build-*.log, *Tests-*.log, final-{Debug,Release}-skeletal-{1,2}.log, viewer-*-debug.log, final-*-fixture.log, final-Debug-component-1.log, results-extra.txt, build-final-results.txt, self-review.txt. 최초 results.txt 끝에 Windows PowerShell Tee-Object의 UTF-16 append가 섞였으므로 최종 빌드 시간은 별도 build-final-results.txt를 사용한다.
- 보호 파일 .mini3개와 BreakConstraintReport.md의 실행 전후 SHA-256 동일. git add/commit/stash/merge/push·메인 체크아웃 수동 수정·junction 변경 없음. 커밋은 바깥 Claude 담당. BC-13a는 시작하지 않았다.

#### BC-12b 화면 증거 보완 (2026-10-30, HEAD b4afd069)

| 단계 | 커밋 | 바뀐 파일 | 테스트·빌드·실행·시간 | 리뷰 |
| --- | --- | --- | --- | --- |
| BC-12b 화면 보완 | 미커밋 | SkeletalPhysicsChecks.h(이번 보완 +61/-1줄), ModelViewer.cpp(+5줄); Implementation §11/12·상태, 이 Progress·상태, Docs/README.md | CPU 두 구성 PASS; 전체 Debug/Release PASS; Xbot 각2회 PASS; Debug fixture·물리OFF·component PASS; 수치는 아래 | 독립 codex home 오류로 실행 불가, 별도 읽기 전용 자기 리뷰 지적0건 |
- 렌더 증거: tick180 실제 GetJoints 팔레트(각 사용 section의 Node14/15, palette11/12)와 최종 node를 실제 ReadDiagnosticState body 원점 frame에서 offset 제거·등록 s 복원한 위치에 각각 비교한다. 독립 ModelInstance animation과의 거리도 각각 검사한다. Debug 전완 palette/body 오차4.92415154e-7m·손1.685873912e-7m, Release 전완9.615579302e-7m·손4.951133405e-7m(허용1e-3m). node/body 최대1.273155732e-7m. animation 거리는 두 구성에서 전완 약1.668m·손 약1.227m(최소0.1m). 각2회 반복 동일·모든 사용 entry PASS. 상완 animation 오차0, 기존 낙하·손목 수치도 이전 BC-12b와 동일하다. 상세 수치는 계약 §12와 로그에 있다.
- CPU: 파일 머리말 cl/Jolt 명령, SkeletalBreak Debug909/909(24.56초)·Release909/909(38.76초), PhysicsBody Debug393건(21.31초)·Release393건(34.55초), 모두 종료0·컴파일 경고0. TEMP 기존 BC12a2-root CPU 폴더 재사용, 이번 obj만 정리해 큰 중복 산출물 없음. CTest 전체 미수행.
- 전체 빌드: worktree 루트 지정 .slnx /m /p:Configuration={Debug,Release} /p:Platform=Windows, /restore 없이 Debug22.36초·Release39.70초 PASS·오류0. BOM 없는 기존 헤더 C4819 각5건(새 include의 SkeletalPhysicsFrames.h 경고1건 포함), vswhere/pwsh fallback 메시지 유지.
- 뷰어: 모두 -debug 1. Xbot Debug60.66/37.23초·Release31.21/28.31초 각2회 BC-12b PASS. Debug -skeletal-break-checks 1 fixture34.86초 BC-12a PASS; 물리OFF 기본(-physics-sim 0 -physics-query-exit 6)61.96초 종료0, 같은 명령에 -component-checks 1 추가40.80초 703 PASS·FAIL0·PASS GPU preparation. 7회 모두 종료0·D3D12 ERROR/CORRUPTION0, debug layer 사용·GPU 기반 검증 미사용. GPU/driver는 기존 NVIDIA MX450/32.0.15.9186 기록이며 이번 재조회 없음. Start-Process -WindowStyle Hidden -Wait 수집기 내부 viewer180초 제한. fixture 최초 수집기 인자 배열이 문자열로 풀려 CreateProcess3으로 시작 전 실패(i-f-x), 스크립트만 수정 후 위34.86초 정상 완료.
- 화면: Xbot 진단에서만 eye=(5.5,-0.15,2.8), at=(5.5,-0.15,0)m 근접 시점. 이번 캡처 시도 없음. Claude 기존 BC12b-screens/t004.5.png·xbot-zoom2.png는 body 낙하 증거이며 메시 피부의 명확한 시각 증거는 아직 아니다. 새 시점에서 전완/손 메시 낙하·팔꿈치 피부 늘어짐·팔꿈치~바닥 화면 높이 절반 이상을 Claude가 확인/캡처한다. 수치 미해결 없음.
- 로그/재현: $env:TEMP/BreakConstraintTests/BC12b-evidence의 verify.ps1, viewers.ps1, fixture.ps1, collect.ps1(화면 캡처 코드 없음), build-{Debug,Release}.log, *Tests-*.log, final-*-skeletal-*.log와 *-debug.log, final-Debug-{fixture,component}-1.log, results.txt/results-extra.txt, self-review.txt. .mini3개·Report SHA-256 전후 동일, git diff --check PASS. git add/commit/stash/merge/push·메인 체크아웃 수동 수정·junction 변경 없음. 다음 단계 미착수.
#### BC-12b 화면 캡처 확인 (Claude, 2026-10-30)

- 방법: Codex 샌드박스 밖에서 Debug `-debug 1 -skeletal-break-xbot 1` 실행 중 PrintWindow로 창을 캡처했다. 종료0.
- 분리 전: `$env:TEMP/BreakConstraintTests/BC12b-screens2/p004.3.png` — 근접 카메라, 왼팔이 몸 옆에 매달려 애니메이션 중, overlay 본·COM·anchor 표시.
- 분리 후: `$env:TEMP/BreakConstraintTests/BC12b-screens2/p006.1.png`, 확대 `after-zoom2.png` — 상완은 팔꿈치 anchor(빨강) 위치에 남고, 전완·손 메시가 바닥으로 떨어져 다리 사이에 놓인다. 렌더 메시가 물리 pose를 따른다(팔레트 수치 검사와 일치).
- 한계: Xbot은 관절마다 조각이 나뉜 마네킹 형태라 팔꿈치 정점이 두 본에 걸친 영역이 작다. 그래서 NDC25 슬라이드처럼 길게 늘어나는 피부 띠는 거의 보이지 않고 전완이 분리되어 보인다. 코드 결함이 아니라 자산 특성이며, 피부가 이어진 캐릭터 자산으로 확인하는 것은 M1 범위 밖 후속이다.
- 독립 리뷰(Codex): BC-12b 구현 지적 0건. 이후 렌더 팔레트 검사·근접 카메라 보완은 Codex 자기 리뷰 지적 0건.
