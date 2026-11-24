# Implementation status

| Task | State | Verification or dependency |
|---|---|---|
| Desktop core, steering, and session logic | Passed | `make test` and `make replay CASE=retreat` |
| Phone-sized replay controls | Passed | `make test-browser` |
| Real-camera tracking score | Pending | Ori footage and matched annotations for held-out evaluation |
| Private GitHub repository | Blocked in this session | GitHub returned HTTP 403 `Resource not accessible by integration` for repository creation |
| Native build, installation, and recovery | Blocked | Exact board and documented native toolchain, install, and restore route |
| Track only on Ori | Pending | Native execution, camera source, and bench access |
| Physical Follow on Ori | Pending | Verified drive expiry, factory ownership and veto interfaces, and measured robot trials |

Highest verified feature milestone is **Desktop replay passed**.
