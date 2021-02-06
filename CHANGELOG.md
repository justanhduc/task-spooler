# CHANGELOG

[1.1.3a] - **Major update (Feb 6, 2021)**

- Supported multiple dependencies.
- Fixed a bug that prints out error message when terminating all processes.
- Added [CHANGELOG](CHANGELOG.md).

[1.1.2] - **Minor update (Feb 4, 2021)**

- Added back option to skip a job if its dependency does not end with exit code 0.
- Jobs can't see GPUs unless the `-G` flag is specified.

[1.1.1] - **Major update (Dec 5, 2020)**

- Using NVML to query GPU usages.
- Selected GPUs randomly.
- Shortened command printed in the interface.
- Added some info to dumped message.
- Improved help.

[1.0.0] - **Major update (Nov 11, 2020)**

- Added GPU support. A GPU job can run or not depending on the number of available GPUs as well as
  free slots.
- Various functionality updates

  - Dependent jobs will run no matter what the error codes of their parents are.
  - Adding more query information about a job.
  - Added long options.

- Replaced Makefile by CMake.

- New task list UI.


[1.0.0]: https://github.com/justanhduc/task-spooler/releases/tag/v1.1.0

[1.1.1]: https://github.com/justanhduc/task-spooler/compare/v1.1.0...v1.1.1

[1.1.2]: https://github.com/justanhduc/task-spooler/compare/v1.1.1...v1.1.2

[1.1.3a]: https://github.com/justanhduc/task-spooler/compare/v1.1.2...v1.1.3a
