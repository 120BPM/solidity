/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Module for applying replacement rules against Expressions.
 */

#include <libyul/optimiser/SimplificationRules.h>

#include <libyul/optimiser/ASTCopier.h>
#include <libyul/optimiser/Semantics.h>
#include <libyul/optimiser/SyntacticalEquality.h>
#include <libyul/backends/evm/EVMDialect.h>
#include <libyul/backends/wasm/WasmDialect.h>
#include <libyul/AsmData.h>
#include <libyul/Utilities.h>

#include <libevmasm/RuleList.h>

using namespace std;
using namespace dev;
using namespace dev::eth;
using namespace langutil;
using namespace yul;


SimplificationRule<yul::Pattern> const* SimplificationRules::findFirstMatch(
	Expression const& _expr,
	Dialect const& _dialect,
	map<YulString, Expression const*> const& _ssaValues
)
{
	static SimplificationRules rules;
	assertThrow(rules.isInitialized(), OptimizerException, "Rule list not properly initialized.");

	auto instruction = instructionAndArguments(_dialect, _expr);
	if (!instruction)
		return nullptr;

	for (auto const& rule: rules.m_rules[uint8_t(instruction->first)])
	{
		rules.resetMatchGroups();
		if (rule.pattern.matches(_expr, _dialect, _ssaValues))
			if (!rule.feasible || rule.feasible())
				return &rule;
	}

	return nullptr;
}

SimplificationRule<yul::PatternEWasm> const* SimplificationRules::findFirstMatchEWasm(
	Expression const& _expr,
	Dialect const& _dialect,
	map<YulString, Expression const*> const& _ssaValues
)
{
	static SimplificationRules rules;
	assertThrow(rules.isInitialized(), OptimizerException, "Rule list not properly initialized.");

	if (_expr.type() == typeid(FunctionCall))
		if (_dialect.builtin(boost::get<FunctionCall>(_expr).functionName.name))
		{
			YulString funName = boost::get<FunctionCall>(_expr).functionName.name;
			for (auto const& rule: rules.m_rulesEWasm[funName])
			{
				rules.resetMatchGroups();
				if (rule.pattern.matches(_expr, _dialect, _ssaValues))
					if (!rule.feasible || rule.feasible())
						return &rule;
			}
		}
	return nullptr;
}

bool SimplificationRules::isInitialized() const
{
	return !m_rules[uint8_t(dev::eth::Instruction::ADD)].empty();
}

boost::optional<std::pair<dev::eth::Instruction, vector<Expression> const*>>
	SimplificationRules::instructionAndArguments(Dialect const& _dialect, Expression const& _expr)
{
	if (_expr.type() == typeid(FunctionalInstruction))
		return make_pair(boost::get<FunctionalInstruction>(_expr).instruction, &boost::get<FunctionalInstruction>(_expr).arguments);
	else if (_expr.type() == typeid(FunctionCall))
		if (auto const* dialect = dynamic_cast<EVMDialect const*>(&_dialect))
			if (auto const* builtin = dialect->builtin(boost::get<FunctionCall>(_expr).functionName.name))
				if (builtin->instruction)
					return make_pair(*builtin->instruction, &boost::get<FunctionCall>(_expr).arguments);

	return {};
}

void SimplificationRules::addRules(vector<SimplificationRule<Pattern>> const& _rules)
{
	for (auto const& r: _rules)
		addRule(r);
}

void SimplificationRules::addRule(SimplificationRule<Pattern> const& _rule)
{
	m_rules[uint8_t(_rule.pattern.instruction())].push_back(_rule);
}

void SimplificationRules::addRule(SimplificationRule<PatternEWasm> const& _rule)
{
	if (!_rule.pattern.builtin().empty())
		m_rulesEWasm[_rule.pattern.builtin()].push_back(_rule);
}

SimplificationRules::SimplificationRules()
{
	{
		// Multiple occurrences of one of these inside one rule must match the same equivalence class.
		// Constants.
		Pattern A(PatternKind::Constant);
		Pattern B(PatternKind::Constant);
		Pattern C(PatternKind::Constant);
		// Anything.
		Pattern W;
		Pattern X;
		Pattern Y;
		Pattern Z;
		A.setMatchGroup(1, m_matchGroups);
		B.setMatchGroup(2, m_matchGroups);
		C.setMatchGroup(3, m_matchGroups);
		W.setMatchGroup(4, m_matchGroups);
		X.setMatchGroup(5, m_matchGroups);
		Y.setMatchGroup(6, m_matchGroups);
		Z.setMatchGroup(7, m_matchGroups);

		addRules(simplificationRuleList(A, B, C, W, X, Y, Z));
	}

	{
		// Multiple occurrences of one of these inside one rule must match the same equivalence class.
		// Constants.
		PatternEWasm A(PatternKind::Constant);
		PatternEWasm B(PatternKind::Constant);
		PatternEWasm C(PatternKind::Constant);
		// Anything.
		PatternEWasm W;
		PatternEWasm X;
		PatternEWasm Y;
		PatternEWasm Z;
		A.setMatchGroup(1, m_matchGroups);
		B.setMatchGroup(2, m_matchGroups);
		C.setMatchGroup(3, m_matchGroups);
		W.setMatchGroup(4, m_matchGroups);
		X.setMatchGroup(5, m_matchGroups);
		Y.setMatchGroup(6, m_matchGroups);
		Z.setMatchGroup(7, m_matchGroups);

		using R = dev::eth::SimplificationRule<PatternEWasm>;
		addRule(R{{"i64.ne"_yulstring, {X, X}}, [=]{ return 0; }, true});
		addRule(R{{"i64.ne"_yulstring, {A, B}}, [=]{ return A.d() != B.d(); }, false});
		addRule(R{{"i64.ne"_yulstring, {X, uint64_t(0)}}, [=]{ return X; }, false});
		addRule(R{{"i64.ne"_yulstring, {uint64_t(0), X}}, [=]{ return X; }, false});
		addRule(R{{"i64.ge_u"_yulstring, {A, B}}, [=]{ return A.d() >= B.d(); }, false});
		addRule(R{{"i64.shl"_yulstring, {A, B}}, [=]() -> uint64_t {
			if (B.d() >= 64)
				return 0;
			return A.d() << B.d();
		}, false});
		addRule(R{{"i64.shr_u"_yulstring, {A, B}}, [=]() -> uint64_t {
			if (B.d() >= 64)
				return 0;
			return A.d() >> B.d();
		}, false});
		for (auto const& r: simplificationRuleList(A, B, C, W, X, Y, Z))
			addRule(r);
	}
	assertThrow(isInitialized(), OptimizerException, "Rule list not properly initialized.");
}

yul::Pattern::Pattern(dev::eth::Instruction _instruction, vector<Pattern> const& _arguments):
	m_kind(PatternKind::Operation),
	m_instruction(_instruction),
	m_arguments(_arguments)
{
}

void Pattern::setMatchGroup(unsigned _group, map<unsigned, Expression const*>& _matchGroups)
{
	m_matchGroup = _group;
	m_matchGroups = &_matchGroups;
}

bool Pattern::matches(
	Expression const& _expr,
	Dialect const& _dialect,
	map<YulString, Expression const*> const& _ssaValues
) const
{
	Expression const* expr = &_expr;

	// Resolve the variable if possible.
	// Do not do it for "Any" because we can check identity better for variables.
	if (m_kind != PatternKind::Any && _expr.type() == typeid(Identifier))
	{
		YulString varName = boost::get<Identifier>(_expr).name;
		if (_ssaValues.count(varName))
			if (Expression const* new_expr = _ssaValues.at(varName))
				expr = new_expr;
	}
	assertThrow(expr, OptimizerException, "");

	if (m_kind == PatternKind::Constant)
	{
		if (expr->type() != typeid(Literal))
			return false;
		Literal const& literal = boost::get<Literal>(*expr);
		if (literal.kind != LiteralKind::Number)
			return false;
		if (m_data && *m_data != u256(literal.value.str()))
			return false;
		assertThrow(m_arguments.empty(), OptimizerException, "");
	}
	else if (m_kind == PatternKind::Operation)
	{
		auto instrAndArgs = SimplificationRules::instructionAndArguments(_dialect, *expr);
		if (!instrAndArgs || m_instruction != instrAndArgs->first)
			return false;
		assertThrow(m_arguments.size() == instrAndArgs->second->size(), OptimizerException, "");
		for (size_t i = 0; i < m_arguments.size(); ++i)
			if (!m_arguments[i].matches(instrAndArgs->second->at(i), _dialect, _ssaValues))
				return false;
	}
	else
	{
		assertThrow(m_arguments.empty(), OptimizerException, "\"Any\" should not have arguments.");
	}

	if (m_matchGroup)
	{
		// We support matching multiple expressions that require the same value
		// based on identical ASTs, which have to be movable.

		// TODO: add tests:
		// - { let x := mload(0) let y := and(x, x) }
		// - { let x := 4 let y := and(x, y) }

		// This code uses `_expr` again for "Any", because we want the comparison to be done
		// on the variables and not their values.
		// The assumption is that CSE or local value numbering has been done prior to this step.

		if (m_matchGroups->count(m_matchGroup))
		{
			assertThrow(m_kind == PatternKind::Any, OptimizerException, "Match group repetition for non-any.");
			Expression const* firstMatch = (*m_matchGroups)[m_matchGroup];
			assertThrow(firstMatch, OptimizerException, "Match set but to null.");
			return
				SyntacticallyEqual{}(*firstMatch, _expr) &&
				SideEffectsCollector(_dialect, _expr).movable();
		}
		else if (m_kind == PatternKind::Any)
			(*m_matchGroups)[m_matchGroup] = &_expr;
		else
		{
			assertThrow(m_kind == PatternKind::Constant, OptimizerException, "Match group set for operation.");
			// We do not use _expr here, because we want the actual number.
			(*m_matchGroups)[m_matchGroup] = expr;
		}
	}
	return true;
}

dev::eth::Instruction Pattern::instruction() const
{
	assertThrow(m_kind == PatternKind::Operation, OptimizerException, "");
	return m_instruction;
}

Expression Pattern::toExpression(SourceLocation const& _location) const
{
	if (matchGroup())
		return ASTCopier().translate(matchGroupValue());
	if (m_kind == PatternKind::Constant)
	{
		assertThrow(m_data, OptimizerException, "No match group and no constant value given.");
		return Literal{_location, LiteralKind::Number, YulString{formatNumber(*m_data)}, {}};
	}
	else if (m_kind == PatternKind::Operation)
	{
		vector<Expression> arguments;
		for (auto const& arg: m_arguments)
			arguments.emplace_back(arg.toExpression(_location));
		// TODO convert to FunctionCall
		string name = instructionInfo(m_instruction).name;
		transform(name.begin(), name.end(), name.begin(), [](unsigned char _c) { return tolower(_c); });

		return FunctionCall{_location, {_location, YulString{name}}, std::move(arguments)};
	}
	assertThrow(false, OptimizerException, "Pattern of kind 'any', but no match group.");
}

u256 Pattern::d() const
{
	return valueOfNumberLiteral(boost::get<Literal>(matchGroupValue()));
}

Expression const& Pattern::matchGroupValue() const
{
	assertThrow(m_matchGroup > 0, OptimizerException, "");
	assertThrow(!!m_matchGroups, OptimizerException, "");
	assertThrow((*m_matchGroups)[m_matchGroup], OptimizerException, "");
	return *(*m_matchGroups)[m_matchGroup];
}


yul::PatternEWasm::PatternEWasm(YulString _builtin, vector<PatternEWasm> const& _arguments):
	m_kind(PatternKind::Operation),
	m_instruction(_builtin),
	m_arguments(_arguments)
{
}

void PatternEWasm::setMatchGroup(unsigned _group, map<unsigned, Expression const*>& _matchGroups)
{
	m_matchGroup = _group;
	m_matchGroups = &_matchGroups;
}

bool PatternEWasm::matches(
	Expression const& _expr,
	Dialect const& _dialect,
	map<YulString, Expression const*> const& _ssaValues
) const
{
	Expression const* expr = &_expr;

	// Resolve the variable if possible.
	// Do not do it for "Any" because we can check identity better for variables.
	if (m_kind != PatternKind::Any && _expr.type() == typeid(Identifier))
	{
		YulString varName = boost::get<Identifier>(_expr).name;
		if (_ssaValues.count(varName))
			if (Expression const* new_expr = _ssaValues.at(varName))
				expr = new_expr;
	}
	assertThrow(expr, OptimizerException, "");

	if (m_kind == PatternKind::Constant)
	{
		if (expr->type() != typeid(Literal))
			return false;
		Literal const& literal = boost::get<Literal>(*expr);
		if (literal.kind != LiteralKind::Number)
			return false;
		// TODO overflow
		if (m_data && *m_data != uint64_t(u256(literal.value.str())))
			return false;
		assertThrow(m_arguments.empty(), OptimizerException, "");
	}
	else if (m_kind == PatternKind::Operation)
	{
		if (expr->type() != typeid(FunctionCall))
			return false;
		if (!_dialect.builtin(boost::get<FunctionCall>(*expr).functionName.name))
			return false;
		if (m_instruction != boost::get<FunctionCall>(*expr).functionName.name)
			return false;
		vector<Expression> const* args = &boost::get<FunctionCall>(*expr).arguments;
		assertThrow(m_arguments.size() == args->size(), OptimizerException, "");
		for (size_t i = 0; i < m_arguments.size(); ++i)
			if (!m_arguments[i].matches(args->at(i), _dialect, _ssaValues))
				return false;
	}
	else
	{
		assertThrow(m_arguments.empty(), OptimizerException, "\"Any\" should not have arguments.");
	}

	if (m_matchGroup)
	{
		// We support matching multiple expressions that require the same value
		// based on identical ASTs, which have to be movable.

		// TODO: add tests:
		// - { let x := mload(0) let y := and(x, x) }
		// - { let x := 4 let y := and(x, y) }

		// This code uses `_expr` again for "Any", because we want the comparison to be done
		// on the variables and not their values.
		// The assumption is that CSE or local value numbering has been done prior to this step.

		if (m_matchGroups->count(m_matchGroup))
		{
			assertThrow(m_kind == PatternKind::Any, OptimizerException, "Match group repetition for non-any.");
			Expression const* firstMatch = (*m_matchGroups)[m_matchGroup];
			assertThrow(firstMatch, OptimizerException, "Match set but to null.");
			return
				SyntacticallyEqual{}(*firstMatch, _expr) &&
				SideEffectsCollector(_dialect, _expr).movable();
		}
		else if (m_kind == PatternKind::Any)
			(*m_matchGroups)[m_matchGroup] = &_expr;
		else
		{
			assertThrow(m_kind == PatternKind::Constant, OptimizerException, "Match group set for operation.");
			// We do not use _expr here, because we want the actual number.
			(*m_matchGroups)[m_matchGroup] = expr;
		}
	}
	return true;
}

YulString PatternEWasm::builtin() const
{
	assertThrow(m_kind == PatternKind::Operation, OptimizerException, "");
	return m_instruction;
}

Expression PatternEWasm::toExpression(SourceLocation const& _location) const
{
	if (matchGroup())
		return ASTCopier().translate(matchGroupValue());
	if (m_kind == PatternKind::Constant)
	{
		assertThrow(m_data, OptimizerException, "No match group and no constant value given.");
		return Literal{_location, LiteralKind::Number, YulString{formatNumber(u256(*m_data))}, {}};
	}
	else if (m_kind == PatternKind::Operation)
	{
		vector<Expression> arguments;
		for (auto const& arg: m_arguments)
			arguments.emplace_back(arg.toExpression(_location));
		return FunctionCall{_location, {_location, m_instruction}, std::move(arguments)};
	}
	assertThrow(false, OptimizerException, "Pattern of kind 'any', but no match group.");
}

uint64_t PatternEWasm::d() const
{
	return uint64_t(valueOfNumberLiteral(boost::get<Literal>(matchGroupValue())));
}

Expression const& PatternEWasm::matchGroupValue() const
{
	assertThrow(m_matchGroup > 0, OptimizerException, "");
	assertThrow(!!m_matchGroups, OptimizerException, "");
	assertThrow((*m_matchGroups)[m_matchGroup], OptimizerException, "");
	return *(*m_matchGroups)[m_matchGroup];
}
