// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "abstract_syntax_list.h"

namespace Shader::IR {

std::string DumpASLNode(const AbstractSyntaxNode& node,
                        const std::map<const Block*, size_t>& block_to_index,
                        const std::map<const Inst*, size_t>& inst_to_index) {
    switch (node.type) {
    case AbstractSyntaxNode::Type::Block:
        return fmt::format("Block: ${}", block_to_index.at(node.data.block));
    case AbstractSyntaxNode::Type::If:
        // gow3 14 Nov 2026: UniformBarrierPass leaves a constant condition.
        return fmt::format("If: cond = {}, body = ${}, merge = ${}",
                           node.data.if_node.cond.IsImmediate()
                               ? std::string{node.data.if_node.cond.U1() ? "true" : "false"}
                               : fmt::format("%{}", inst_to_index.at(node.data.if_node.cond.Inst())),
                           block_to_index.at(node.data.if_node.body),
                           block_to_index.at(node.data.if_node.merge));
    case AbstractSyntaxNode::Type::EndIf:
        return fmt::format("EndIf: merge = ${}", block_to_index.at(node.data.end_if.merge));
    case AbstractSyntaxNode::Type::Loop:
        return fmt::format("Loop: body = ${}, continue = ${}, merge = ${}",
                           block_to_index.at(node.data.loop.body),
                           block_to_index.at(node.data.loop.continue_block),
                           block_to_index.at(node.data.loop.merge));
    case AbstractSyntaxNode::Type::Repeat:
        return fmt::format("Repeat: cond = %{}, header = ${}, merge = ${}",
                           inst_to_index.at(node.data.repeat.cond.Inst()),
                           block_to_index.at(node.data.repeat.loop_header),
                           block_to_index.at(node.data.repeat.merge));
    case AbstractSyntaxNode::Type::Break:
        return fmt::format("Break: cond = %{}, merge = ${}, skip = ${}",
                           inst_to_index.at(node.data.break_node.cond.Inst()),
                           block_to_index.at(node.data.break_node.merge),
                           block_to_index.at(node.data.break_node.skip));
    case AbstractSyntaxNode::Type::Return:
        return "Return";
    case AbstractSyntaxNode::Type::Unreachable:
        return "Unreachable";
    };
    UNREACHABLE();
}

} // namespace Shader::IR