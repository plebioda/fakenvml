#Contributing to NVML
The NVML project welcomes new contributors. This document will guide you through the process.

You may also wish to join our [Google Group](http://groups.google.com/group/pmem)
or find us on
IRC on the **#pmem** channel on [OFTC](http://www.oftc.net).

##Code Contributions
We welcome contributions to NVML. **IMPORTANT**: By submitting a patch, you agree to allow the project owners to license your work under the terms of the 
[NVML License](https://github.com/pmem/nvml/blob/master/LICENSE).

###Development
See the Workflow blog article: http://pmem.io/2014/09/09/git-workflow.html

If you are considering adding a feature to NVML, please let other developers know by creating an issue here: https://github.com/pmem/issues.
Add the "Type: Feature" label, a comment describing the feature, and assign it to yourself.

##Bug Reports
Bugs for the NVML project are tracked in https://github.com/pmem/issues.

When creating a bug report issue for NVML, please provide the following information:
###Milestone field
Put the release name of the version of NVML running when the bug was discovered in the Milestone field. If you saw this bug in multiple NVML versions, please put the most recent version in Milestone and list the others in a bug comment.
- Stable release names are in the form v#.# (where # represents an integer); for example v0.3.
- Release names from working versions look like v#.#+b# (adding a build #) or v#.#-rc# (adding a release candidate number)
If NVML was built from source, the version number can be retrieved from git using this command: `git describe --tags`

For binary NVML releases, use the entire package name. For RPMs, this can retrieved as follows:
 `rpm -q nvml`

For Deb packages, run `dpkg-query -W nvml` and use the second (version) string.

###Type: Bug label
Assign the Type: Bug label to the issue (see https://help.github.com/articles/applying-labels-to-issues-and-pull-requests )
###Priority label

Optionally, assign one of the Priority labels (P1, P2, ...). See https://help.github.com/articles/applying-labels-to-issues-and-pull-requests. Priority attribute represents the determination of the urgency to resolve a defect and establishes the timeframe for providing a verified resolution, respectively. These Priority labels are defined as:

**P1**: Resolution of this defect takes precedence over other defects and most other development activities. This level is used to focus maximum team resources to resolve a defect in the shortest possible timeframe. The timeframe to resolve P1 priority defects is always immediate and may force an unplanned release to customers to resolve the defect.

**P2**: Resolution of the defect has precedence over resolving other defects with lesser classifications of priority. P2 priority defects are intended to be resolved by the next planned external release of the software.

**P3**: Resolution of the defect has precedence over resolving other defects with lesser classifications of priority. P3 priority defects must have a planned timeframe for a verified resolution.

**P4**: Resolution of the defect has the least urgency to resolve. P4 priority defects may or may not have documented plans to resolve.

Then describe the bug in comment fields.

##Feature Requests
###Type: Feature label
Feature requests for the NVML project are tracked in https://github.com/pmem/issues.

Assign the Type: Feature label to the issue (see https://help.github.com/articles/applying-labels-to-issues-and-pull-requests )

Then describe the feature request in comment fields.

If you plan to implement the feature, make sure this is reflected in a commant and assign the issue to yourself.
