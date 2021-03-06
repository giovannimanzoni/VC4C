/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "TypeConversions.h"

using namespace vc4c;
using namespace vc4c::intermediate;

InstructionWalker intermediate::insertZeroExtension(InstructionWalker it, Method& method, const Value& src, const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
	if(src.type.getScalarBitCount() == 32 && dest.type.getScalarBitCount() <= 32)
	{
		//"extend" to smaller type
		it.emplace(new MoveOperation(dest, src, conditional, setFlags));
		switch(dest.type.getScalarBitCount())
		{
			case 8:
				it->setPackMode(PACK_INT_TO_CHAR_TRUNCATE);
				break;
			case 16:
				it->setPackMode(PACK_INT_TO_SHORT_TRUNCATE);
				break;
			case 32:
				//no pack mode
				break;
			default:
				throw CompilationError(CompilationStep::GENERAL, "Invalid type-width for zero-extension", dest.type.to_string());
		}
	}
	else if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        //do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace( new MoveOperation(dest, src, conditional, setFlags));
    }
	else if(dest.type.getScalarBitCount() == 32 && src.hasType(ValueType::REGISTER) &&
			(has_flag(src.reg.file, RegisterFile::PHYSICAL_A) || has_flag(src.reg.file, RegisterFile::ACCUMULATOR)) &&
			src.type.getScalarBitCount() == 8)
	{
		//if we zero-extend from register-file A, use unpack-modes
		//this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
		it.emplace(new MoveOperation(dest, src, conditional, setFlags));
		it->setUnpackMode(UNPACK_CHAR_TO_INT_ZEXT);
	}
	else if(allowLiteral)
	{
		it.emplace(new Operation(OP_AND, dest, src, Value(Literal(src.type.getScalarWidthMask()), TYPE_INT32), conditional, setFlags));
	}
    else
    {
    	const Value tmp = method.addNewLocal(TYPE_INT32, "%zext");
    	it.emplace(new LoadImmediate(tmp, Literal(src.type.getScalarWidthMask())));
    	it.nextInBlock();
    	it.emplace( new Operation(OP_AND, dest, src, tmp, conditional, setFlags));
    }

    it->decoration = add_flag(it->decoration, InstructionDecorations::UNSIGNED_RESULT);
    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSignExtension(InstructionWalker it, Method& method, const Value& src, const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
	if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        //do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace( new MoveOperation(dest, src, conditional, setFlags));
    }
	else if(dest.type.getScalarBitCount() == 32 && src.hasType(ValueType::REGISTER) &&
			(has_flag(src.reg.file, RegisterFile::PHYSICAL_A) || has_flag(src.reg.file, RegisterFile::ACCUMULATOR)) &&
			src.type.getScalarBitCount() == 16)
	{
		//if we sign-extend from register-file A, use unpack-modes
		//this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
		it.emplace(new MoveOperation(dest, src, conditional, setFlags));
		it->setUnpackMode(UNPACK_SHORT_TO_INT_SEXT);
	}
	else
	{

		// out = asr(shl(in, bit_diff) bit_diff)
		Value widthDiff(Literal(static_cast<int64_t>(dest.type.getScalarBitCount() - src.type.getScalarBitCount())), TYPE_INT8);

		if(!allowLiteral)
		{
			Value tmp = method.addNewLocal(TYPE_INT8, "%sext");
			it.emplace(new LoadImmediate(tmp, widthDiff.literal));
			it.nextInBlock();

			widthDiff = tmp;
		}

		const Value tmp = method.addNewLocal(TYPE_INT32, "%sext");
		it.emplace( new Operation(OP_SHL, tmp, src, widthDiff, conditional));
		it.nextInBlock();
		it.emplace( new Operation(OP_ASR, dest, tmp, widthDiff, conditional, setFlags));
	}

    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSaturation(InstructionWalker it, Method& method, const Value& src, const Value& dest, bool isSigned)
{
	//saturation = clamping to min/max of type
	//-> dest = max(min(src, destType.max), destType.min)
	//-> or via pack-modes

	if(dest.type.complexType || dest.type.isFloatingType())
		throw CompilationError(CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());

	if(src.hasType(ValueType::LITERAL))
	{
		switch(dest.type.getScalarBitCount())
		{
			case 8:
				return it.emplace(new MoveOperation(dest, Value(Literal(isSigned ? saturate<int8_t>(src.literal.integer) : saturate<uint8_t>(src.literal.integer)), dest.type)));
			case 16:
				return it.emplace(new MoveOperation(dest, Value(Literal(isSigned ? saturate<int16_t>(src.literal.integer) : saturate<uint16_t>(src.literal.integer)), dest.type)));
			case 32:
				return it.emplace(new MoveOperation(dest, Value(Literal(isSigned ? saturate<int32_t>(src.literal.integer) : saturate<uint32_t>(src.literal.integer)), dest.type)));
			default:
				throw CompilationError(CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());
		}
	}
	else	//saturation can be easily done via pack-modes
	{
		if(dest.type.getScalarBitCount() == 8 && !isSigned)
			return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_INT_TO_UNSIGNED_CHAR_SATURATE));
		else if(dest.type.getScalarBitCount() == 16 && isSigned)
			return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_INT_TO_SIGNED_SHORT_SATURATE));
		else if(dest.type.getScalarBitCount() == 32)
			return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_32_32));
		//TODO need to saturate manually
		throw CompilationError(CompilationStep::GENERAL, "Saturation to this type is not yet supported", dest.type.to_string());
	}
}

InstructionWalker intermediate::insertTruncate(InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
	if(dest.type.getScalarBitCount() >= src.type.getScalarBitCount())
		//"truncate" to larger type, simply move
		it.emplace(new MoveOperation(dest, src));
	else
		it.emplace(new Operation(OP_AND, dest, src, Value(Literal(dest.type.getScalarWidthMask()), TYPE_INT32)));

	return it.nextInBlock();
}
