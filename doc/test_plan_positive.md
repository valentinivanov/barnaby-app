i would like to create a test suite for the gitboard. For now it will test only positive cases and won't test git-related functions. 

The test should be a separate executable and it should control the gitboard executable. 
In the test pass the test executable must: 
for each call provide a root path to some temp folder in the system temp directory. If any call returns an error - test is failed and should not proceed. Should display the appropriate message.
The test flow: 
1) run the create command, parse result, ensure the file is created, store the task id.
2) run the create command, parse result, ensure the file is created, store the task id 
3) compare task ids from 1) and 2) and make sure they are different 
4) make sure both tasks have non-default created_at fields
For next steps it would make sense to create a in-memory task object, update its fields, then update on-disk task, load it and compare to the in-memory one.
For each next step check the updated_at field value: it must be within 10 seconds from the current time.
5) run the body command for the task created on 1) and provide some random string properly encoded as the body value. Store the body value in the in-memory task.
6) run the task command, decode result. Make sure the loaded task has the body value matching the in-memory task body value.
7) run the assignee command for the task created on 1) and provide some random string properly encoded as the assignee value. Store the assignee value in the in-memory task.
8) run the task command, decode result. Make sure the loaded task has the assignee value matching the in-memory task assignee value.
9) run the branches command for the task created on 1) and provide some random value properly encoded as the branches value. Store the branches value in the in-memory task.
10) run the task command, decode result. Make sure the loaded task has the branches value matching the in-memory task branches value.
11) run the ci_status command for the task created on 1) and provide some random string properly encoded as the ci_status value. Store the ci_status value in the in-memory task.
12) run the task command, decode result. Make sure the loaded task has the ci_status value matching the in-memory task ci_status value.
13) run the priority command for the task created on 1) and provide some random string properly encoded as the priority value. Store the priority value in the in-memory task.
14) run the task command, decode result. Make sure the loaded task has the priority value matching the in-memory priority value.
15) run the prs command for the task created on 1) and provide some random value properly encoded as the prs value. Store the prs value in the in-memory task.
16) run the task command, decode result. Make sure the loaded task has the prs value matching the in-memory prs value.
17) run the tags command for the task created on 1) and provide some random value properly encoded as the tags value. Store the tags value in the in-memory task.
18) run the task command, decode result. Make sure the loaded task has the tags value matching the in-memory task tags value.
19) run the title command for the task created on 1) and provide some random string properly encoded as the title value. Store the title value in the in-memory task.
20) run the task command, decode result. Make sure the loaded task has the title value matching the in-memory task title value.
21) run the move command for the task created on 1) and provide proper status according to the config properly encoded as the status value. Store the status value in the in-memory task.
22) run the task command, decode result. Make sure the loaded task has the status value matching the in-memory task status value.
23) run the comment command for the task created on 1) and provide some random string properly encoded as the comment value. Store the comment value in the in-memory task.
24) run the task command, decode result. Make sure the loaded task has the comment value matching the in-memory task comment value.
25) Run the task command, decode result. Compare the loaded task and the in-memory tasl field by field. All fields must match except created_at and updated_at.
26) run the batch command. Batch the following commands: list, task for task from 1), task from task from 2). Make sure the batch result is valid.

Once the test is finished (success or fail) delete task files, delete the temporary working folder.
