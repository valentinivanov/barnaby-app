---
assignee: cody
branches: []
ci_status: unknown
created_at: 2026-06-30T19:02:50.226262Z
created_by: ''
id: TASK-024
priority: high
prs: []
status: done
story_points: 5
tags:
- git
- automation
title: '🗂️ Pull task changes from Git – enable Barnaby to sync changes from git without external tools'
updated_at: 2026-07-01T17:44:00.044397Z
---

Barnaby should be able to pull task changes from git without using external tools. 

This includes fetching updated task definitions, reconciling status, and integrating with the repository while ensuring data consistency and proper version control.

The sync procedure should happen when opening the repo, when opening the app with the repo opened or when the Refresh button is pressed. 

Also Barnaby should check git periodically when running with fetch and add indication to the Refresh button if tasks folder has changed from the last time and siaplay yellow status message.

Barnaby should fetch changes from git and check if the tasks folder has any modifications in the repo. In this case in case when 
- opening the repo 
- opening the app with the repo opened 
- the Refresh button is pressed
- automatic check performed
Barnaby should not pull changes automatically. Instead it should add a status message with yellow background saying “Tasks in repo have changes please refresh your local copy as soon as possible”. Also it should add a red outline to the Refresh button. Also it should add transparency to the modified in repo task cards and disable user interaction with them. Also it should display newly added cards in the backlog column as a semitransparent ones and the user shoyldnot be able to interact with them. Since we can only see names of new tasks don’t display ids in the colored header and display file name as task description.
