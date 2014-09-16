/*
   Copyright 2014 Software Reliability Lab, ETH Zurich

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include "TracePreprocess.h"

#include <stddef.h>
#include <stdio.h>
#include <map>

void TracePreprocess::RemoveEmptyReadWrites() {
	int num_ops = m_log->maxEventActionId()  + 1;
	for (int op_id = 0; op_id < num_ops; ++op_id) {
		if (m_log->event_action(op_id).m_commands.empty()) continue;  // Avoid adding event actions.
		ActionLog::EventAction* op = m_log->mutable_event_action(op_id);
		for (size_t cmd_id = 3; cmd_id < op->m_commands.size(); ++cmd_id) {

			ActionLog::Command& cmd0 = op->m_commands[cmd_id - 3];
			if (cmd0.m_cmdType != ActionLog::READ_MEMORY) continue;
			ActionLog::Command& cmd1 = op->m_commands[cmd_id - 2];
			if (cmd1.m_cmdType != ActionLog::MEMORY_VALUE) continue;

			ActionLog::Command& cmd2 = op->m_commands[cmd_id - 1];
			if (cmd2.m_cmdType != ActionLog::WRITE_MEMORY) continue;
			if (cmd2.m_location != cmd0.m_location) continue;
			ActionLog::Command& cmd3 = op->m_commands[cmd_id - 0];
			if (cmd3.m_cmdType != ActionLog::MEMORY_VALUE) continue;
			if (cmd3.m_location != cmd1.m_location) continue;

			// Mark the four operations for deletions.
			cmd0.m_cmdType = cmd1.m_cmdType = cmd2.m_cmdType = cmd3.m_cmdType = static_cast<ActionLog::CommandType>(-1);
		}
	}
	RemoveEmptyOperations();
}

void TracePreprocess::RemoveNopWrites() {
	std::map<int, int> last_written_value;

	int num_ops = m_log->maxEventActionId()  + 1;
	for (int op_id = 0; op_id < num_ops; ++op_id) {
		if (m_log->event_action(op_id).m_commands.empty()) continue;  // Avoid adding event actions.
		ActionLog::EventAction* op = m_log->mutable_event_action(op_id);

		for (size_t cmd_id = 1; cmd_id < op->m_commands.size(); ++cmd_id) {
			ActionLog::Command& cmd0 = op->m_commands[cmd_id - 1];
			if (cmd0.m_cmdType != ActionLog::READ_MEMORY && cmd0.m_cmdType != ActionLog::WRITE_MEMORY) continue;
			ActionLog::Command& cmd1 = op->m_commands[cmd_id - 0];
			if (cmd1.m_cmdType != ActionLog::MEMORY_VALUE) continue;

			int memory_location = cmd0.m_location;
			int value = cmd1.m_location;

			if (cmd0.m_cmdType == ActionLog::WRITE_MEMORY &&
					last_written_value.find(memory_location) != last_written_value.end() &&
					last_written_value[memory_location] == value) {
				// A write that does not change the value.

				// Mark the write operation for deletions.
				cmd0.m_cmdType = cmd1.m_cmdType = static_cast<ActionLog::CommandType>(-1);
			}
			last_written_value[memory_location] = value;  // record the last known value for the memory location.
		}
	}
	RemoveEmptyOperations();
}

void TracePreprocess::RemoveUpdatesInSameMethod() {
	int num_ops = m_log->maxEventActionId() + 1;

	// For each memory location, keep in track the function it was initialized in.
	// We set it to -1 if we detect initialization in multiple methods.
	std::map<int, int> initialization_location;

	for (int op_id = 0; op_id < num_ops; ++op_id) {
		if (m_log->event_action(op_id).m_commands.empty()) continue;  // Avoid adding event actions.
		ActionLog::EventAction* op = m_log->mutable_event_action(op_id);
		std::vector<int> scope;

		std::map<int, int> mem_state;  // State of each memory location within an event action.
		for (size_t cmd_id = 1; cmd_id < op->m_commands.size(); ++cmd_id) {
			ActionLog::Command& cmd0 = op->m_commands[cmd_id - 1];
			if (cmd0.m_cmdType == ActionLog::ENTER_SCOPE) {
				scope.push_back(cmd0.m_location);
				continue;
			}
			if (cmd0.m_cmdType == ActionLog::EXIT_SCOPE) {
				if (!scope.empty()) scope.pop_back();
				continue;
			}

			if (cmd0.m_cmdType != ActionLog::READ_MEMORY && cmd0.m_cmdType != ActionLog::WRITE_MEMORY) continue;
			ActionLog::Command& cmd1 = op->m_commands[cmd_id - 0];
			if (cmd1.m_cmdType != ActionLog::MEMORY_VALUE) continue;
			int memory_location = cmd0.m_location;

			if (initialization_location.find(memory_location) != initialization_location.end() &&
					!scope.empty() &&
					initialization_location[memory_location] == scope.back()) {
				// Mark the read or write operation for deletion.
				cmd0.m_cmdType = cmd1.m_cmdType = static_cast<ActionLog::CommandType>(-1);
				continue;
			}

			// First detect a read.
			if (cmd0.m_cmdType == ActionLog::READ_MEMORY) {
				if (mem_state[memory_location] != 0) {
					mem_state[memory_location] = -1;
					continue;
				}
				mem_state[memory_location] = 1;  // State = read was present.
			}
			// Require that the read is followed by a write.
			if (cmd0.m_cmdType == ActionLog::WRITE_MEMORY) {
				if (mem_state[memory_location] != 1) {
					mem_state[memory_location] = -1;
					continue;
				}
				mem_state[memory_location] = 2;  // State = write was present after a read.
				if (initialization_location[memory_location] == 0 && !scope.empty()) {
					initialization_location[memory_location] = scope.back();
				} else {
					initialization_location[memory_location] = -1;
				}
			}
		}
	}
	RemoveEmptyOperations();
}

void TracePreprocess::RemoveEmptyOperations() {
	int num_ops = m_log->maxEventActionId()  + 1;
	for (int op_id = 0; op_id < num_ops; ++op_id) {
		if (m_log->event_action(op_id).m_commands.empty()) continue;  // Avoid adding event actions.
		ActionLog::EventAction* op = m_log->mutable_event_action(op_id);
		size_t new_cmd_id = 0;
		for (size_t cmd_id = 0; cmd_id < op->m_commands.size(); ++cmd_id) {
			const ActionLog::Command& cmd = op->m_commands[cmd_id];
			if (cmd_id != new_cmd_id) {
				op->m_commands[new_cmd_id] = cmd;
			}
			if (cmd.m_cmdType != -1) {
				++new_cmd_id;
			}
		}
		op->m_commands.erase(op->m_commands.begin() + new_cmd_id, op->m_commands.end());
	}
}



