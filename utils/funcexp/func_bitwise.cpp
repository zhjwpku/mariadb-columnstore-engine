/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

/****************************************************************************
* $Id: func_bitwise.cpp 3616 2013-03-04 14:56:29Z rdempsey $
*
*
****************************************************************************/

#include <string>
using namespace std;

#include "functor_int.h"
#include "funchelpers.h"
#include "functioncolumn.h"
#include "predicateoperator.h"
using namespace execplan;

#include "rowgroup.h"
using namespace rowgroup;

#include "errorcodes.h"
#include "idberrorinfo.h"
#include "errorids.h"
using namespace logging;

#include "mcs_int64.h"
#include "mcs_decimal.h"
#include "dataconvert.h"
using namespace dataconvert;

namespace
{
using namespace funcexp;


template<typename T>
datatypes::TUInt64Null ConvertToBitOperand(const T &val)
{
    if (val > static_cast<T>(UINT64_MAX))
        return datatypes::TUInt64Null(UINT64_MAX);
    if (val >= 0)
        return datatypes::TUInt64Null(static_cast<uint64_t>(val));
    if (val < static_cast<T>(INT64_MIN))
        return datatypes::TUInt64Null(static_cast<uint64_t>(INT64_MAX)+1);
    return datatypes::TUInt64Null((uint64_t) (int64_t) val);
}


static
datatypes::TUInt64Null DecimalToBitOperand(Row&  row,
                                           const execplan::SPTP& parm,
                                           const funcexp::Func& thisFunc)
{
    bool tmpIsNull = false;
    IDB_Decimal d = parm->data()->getDecimalVal(row, tmpIsNull);
    if (tmpIsNull)
        return datatypes::TUInt64Null();

    if (IDB_Decimal::isWideDecimalTypeByPrecision(d.precision))
    {
        int128_t val = d.getPosNegRoundedIntegralPart(0).getValue();
        return ConvertToBitOperand<int128_t>(val);
    }

    return datatypes::TUInt64Null((uint64_t) d.narrowRound());
}


// @bug 4703 - the actual bug was only in the DATETIME case
// part of this statement below, but instead of leaving 5 identical
// copies of this code, extracted into a single utility function
// here.  This same method is potentially useful in other methods
// and could be extracted into a utility class with its own header
// if that is the case - this is left as future exercise
datatypes::TUInt64Null GenericToBitOperand(
    Row&  row,
    const execplan::SPTP& parm,
    const funcexp::Func& thisFunc,
    bool temporalRounding)
{
    switch (parm->data()->resultType().colDataType)
    {
        case execplan::CalpontSystemCatalog::BIGINT:
        case execplan::CalpontSystemCatalog::INT:
        case execplan::CalpontSystemCatalog::MEDINT:
        case execplan::CalpontSystemCatalog::TINYINT:
        case execplan::CalpontSystemCatalog::SMALLINT:
        {
            datatypes::TSInt64Null tmp= parm->data()->toTSInt64Null(row);
            return tmp.isNull() ? datatypes::TUInt64Null() :
                                  datatypes::TUInt64Null((uint64_t) (int64_t) tmp);
        }
        case execplan::CalpontSystemCatalog::DOUBLE:
        case execplan::CalpontSystemCatalog::FLOAT:
        case execplan::CalpontSystemCatalog::UDOUBLE:
        case execplan::CalpontSystemCatalog::UFLOAT:
        {
            bool tmpIsNull;
            double val = parm->data()->getDoubleVal(row, tmpIsNull);
            return tmpIsNull ? datatypes::TUInt64Null() :
                               ConvertToBitOperand<double>(val);
        }

        case execplan::CalpontSystemCatalog::UBIGINT:
        case execplan::CalpontSystemCatalog::UINT:
        case execplan::CalpontSystemCatalog::UMEDINT:
        case execplan::CalpontSystemCatalog::UTINYINT:
        case execplan::CalpontSystemCatalog::USMALLINT:
            return parm->data()->toTUInt64Null(row);

        case execplan::CalpontSystemCatalog::VARCHAR:
        case execplan::CalpontSystemCatalog::CHAR:
        case execplan::CalpontSystemCatalog::TEXT:
        {
            bool tmpIsNull;
            const string& str = parm->data()->getStrVal(row, tmpIsNull);
            if (tmpIsNull)
                return datatypes::TUInt64Null();
            static const datatypes::SystemCatalog::TypeAttributesStd
              attr(datatypes::MAXDECIMALWIDTH, 6, datatypes::INT128MAXPRECISION);
            int128_t val = attr.decimal128FromString(str);
            datatypes::Decimal d(0, attr.scale, attr.precision, &val);
            val = d.getPosNegRoundedIntegralPart(0).getValue();
            return ConvertToBitOperand<int128_t>(val);
        }

        case execplan::CalpontSystemCatalog::DECIMAL:
        case execplan::CalpontSystemCatalog::UDECIMAL:
            return DecimalToBitOperand(row, parm, thisFunc);

        case execplan::CalpontSystemCatalog::DATE:
        {
            bool tmpIsNull;
            int32_t time = parm->data()->getDateIntVal(row, tmpIsNull);
            if (tmpIsNull)
                return datatypes::TUInt64Null();

            int64_t value = Date(time).convertToMySQLint();
            return datatypes::TUInt64Null((uint64_t) value);
        }

        case execplan::CalpontSystemCatalog::DATETIME:
        {
            bool tmpIsNull;
            int64_t time = parm->data()->getDatetimeIntVal(row, tmpIsNull);
            if (tmpIsNull)
                return datatypes::TUInt64Null();

            // @bug 4703 - missing year when convering to int
            DateTime dt(time);
            int64_t value = dt.convertToMySQLint();
            if (temporalRounding && dt.msecond >= 500000)
                value++;
            return datatypes::TUInt64Null((uint64_t) value);
        }

        case execplan::CalpontSystemCatalog::TIMESTAMP:
        {
            bool tmpIsNull;
            int64_t time = parm->data()->getTimestampIntVal(row, tmpIsNull);
            if (tmpIsNull)
                return datatypes::TUInt64Null();

            TimeStamp dt(time);
            int64_t value = dt.convertToMySQLint(thisFunc.timeZone());
            if (temporalRounding && dt.msecond >= 500000)
                value++;
            return datatypes::TUInt64Null((uint64_t) value);
        }

        case execplan::CalpontSystemCatalog::TIME:
        {
            bool tmpIsNull;
            int64_t time = parm->data()->getTimeIntVal(row, tmpIsNull);

            Time dt(time);
            int64_t value = dt.convertToMySQLint();
            if (temporalRounding && dt.msecond >= 500000)
              value < 0 ? value-- : value++;
            return datatypes::TUInt64Null((uint64_t) value);
        }

        default:
            break;
    }

    return datatypes::TUInt64Null();
}

}

namespace funcexp
{


bool Func_BitOp::fixForBitOp1(execplan::FunctionColumn &col,
                              Func_Int & return_uint64_from_uint64,
                              Func_Int & return_uint64_from_sint64) const
{
    if (col.functionParms()[0]->data()->resultType().isUnsignedInteger())
    {
        col.setFunctor(&return_uint64_from_uint64);
        return false;
    }
    if (col.functionParms()[0]->data()->resultType().isSignedInteger())
    {
        col.setFunctor(&return_uint64_from_sint64);
        return false;
    }
    return false; // Keep generic functor
}


bool Func_BitOp::fixForBitOp2(execplan::FunctionColumn &col,
                              Func_Int & return_uint64_from_uint64_uint64,
                              Func_Int & return_uint64_from_sint64_sint64) const
{
    if (col.functionParms()[0]->data()->resultType().isUnsignedInteger() &&
        col.functionParms()[1]->data()->resultType().isUnsignedInteger())
    {
         col.setFunctor(&return_uint64_from_uint64_uint64);
         return false;
    }
    if (col.functionParms()[0]->data()->resultType().isSignedInteger() &&
        col.functionParms()[1]->data()->resultType().isSignedInteger())
    {
         col.setFunctor(&return_uint64_from_sint64_sint64);
         return false;
    }
    return false; // Keep generic functor
}


class BitOperandTSInt64: public datatypes::TSInt64Null
{
public:
    BitOperandTSInt64() { }
    BitOperandTSInt64(Row& row,
                      const execplan::SPTP& parm,
                      const funcexp::Func& thisFunc)
       :TSInt64Null(parm->data()->toTSInt64Null(row))
    { }
    operator uint64_t () const
    {
        idbassert(!isNull());
        return (uint64_t) mValue;
    }
};


class BitOperandTUInt64: public datatypes::TUInt64Null
{
public:
    BitOperandTUInt64() { }
    BitOperandTUInt64(Row& row,
                      const execplan::SPTP& parm,
                      const funcexp::Func& thisFunc)
       :TUInt64Null(parm->data()->toTUInt64Null(row))
    { }
};


class BitOperandGeneric: public datatypes::TUInt64Null
{
public:
    BitOperandGeneric() { }
    BitOperandGeneric(Row& row,
                      const execplan::SPTP& parm,
                      const funcexp::Func& thisFunc)
       :TUInt64Null(GenericToBitOperand(row, parm, thisFunc, true))
    { }
};


// The shift amount operand in MariaDB does not round temporal values.
// QQ: this should probably be fixed in MariaDB.
class BitOperandGenericShiftAmount: public datatypes::TUInt64Null
{
public:
    BitOperandGenericShiftAmount() { }
    BitOperandGenericShiftAmount(Row& row,
                                 const execplan::SPTP& parm,
                                 const funcexp::Func& thisFunc)
       :TUInt64Null(GenericToBitOperand(row, parm, thisFunc, false))
    { }
};


template<class TA, class TB> class PairLazy
{
public:
   TA a;
   TB b;
   PairLazy(Row& row,
            FunctionParm& parm,
            const funcexp::Func& thisFunc)
      :a(row, parm[0], thisFunc),
       b(a.isNull() ? TB() : TB(row, parm[1], thisFunc))
   { }
   bool isNull() const { return a.isNull() || b.isNull(); }
   uint64_t bitAnd(bool & isNullRef) const
   {
       return (isNullRef = isNull()) ? 0 : ((uint64_t) a & (uint64_t) b);
   }
   uint64_t bitOr(bool & isNullRef) const
   {
       return (isNullRef = isNull()) ? 0 : ((uint64_t) a | (uint64_t) b);
   }
};


template<class TA, class TB> class PairEager
{
public:
   TA a;
   TB b;
   PairEager(Row& row,
            FunctionParm& parm,
            const funcexp::Func& thisFunc)
      :a(row, parm[0], thisFunc),
       b(row, parm[1], thisFunc)
   { }
   bool isNull() const { return a.isNull() || b.isNull(); }
   uint64_t shiftLeft(bool & isNullRef) const
   {
       return ((isNullRef = isNull()) || ((uint64_t) b) > 64) ? 0 :
              (uint64_t) a << (uint64_t) b;
   }
   uint64_t shiftRight(bool & isNullRef) const
   {
       return ((isNullRef = isNull()) || ((uint64_t) b) > 64) ? 0 :
              (uint64_t) a >> (uint64_t) b;
   }
   uint64_t bitXor(bool & isNullRef) const
   {
       return (isNullRef = isNull()) ? 0 : ((uint64_t) a ^ (uint64_t) b);
   }
};


//
// BITAND
//


// uint64 = generic & generic
int64_t Func_bitand::getIntVal(Row& row,
                               FunctionParm& parm,
                               bool& isNull,
                               CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 2);
    PairLazy<BitOperandGeneric, BitOperandGeneric> args(row, parm, *this);
    return (int64_t) args.bitAnd(isNull);
}


// uint64 = uint64 & uint64
class Func_bitand_return_uint64_from_uint64_uint64: public Func_bitand
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairLazy<BitOperandTUInt64, BitOperandTUInt64> args(row, parm, *this);
        return (int64_t) args.bitAnd(isNull);
    }
};


// uint64 = sint64 & sint64
class Func_bitand_return_uint64_from_sint64_sint64: public Func_bitand
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairLazy<BitOperandTSInt64, BitOperandTSInt64> args(row, parm, *this);
        return (int64_t) args.bitAnd(isNull);
    }
};


bool Func_bitand::fix(execplan::FunctionColumn &col) const
{
    static Func_bitand_return_uint64_from_uint64_uint64 return_uint64_from_uint64_uint64;
    static Func_bitand_return_uint64_from_sint64_sint64 return_uint64_from_sint64_sint64;
    return fixForBitOp2(col, return_uint64_from_uint64_uint64,
                             return_uint64_from_sint64_sint64);
}


//
// LEFT SHIFT
//


// uint64 = generic << generic_shift_amount
int64_t Func_leftshift::getIntVal(Row& row,
                                  FunctionParm& parm,
                                  bool& isNull,
                                  CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 2);
    PairEager<BitOperandGeneric, BitOperandGenericShiftAmount> args(row, parm, *this);
    return (int64_t) args.shiftLeft(isNull);
}


// uint64 = uint64 << generic_shift_amount
class Func_leftshift_return_uint64_from_uint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTUInt64, BitOperandGenericShiftAmount> args(row, parm, *this);
        return (int64_t) args.shiftLeft(isNull);
    }
};


// uint64 = sint64 << generic_shift_amount
class Func_leftshift_return_uint64_from_sint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTSInt64, BitOperandGenericShiftAmount> args(row, parm, *this);
        return (int64_t) args.shiftLeft(isNull);
    }
};


bool Func_leftshift::fix(execplan::FunctionColumn &col) const
{
    static Func_leftshift_return_uint64_from_uint64 return_uint64_from_uint64;
    static Func_leftshift_return_uint64_from_sint64 return_uint64_from_sint64;
    return fixForBitOp1(col, return_uint64_from_uint64,
                             return_uint64_from_sint64);
}


//
// RIGHT SHIFT
//


int64_t Func_rightshift::getIntVal(Row& row,
                                   FunctionParm& parm,
                                   bool& isNull,
                                   CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 2);
    PairEager<BitOperandGeneric, BitOperandGenericShiftAmount> args(row, parm, *this);
    return (int64_t) args.shiftRight(isNull);
}


// uint64 = uint64 << generic_shift_amount
class Func_rightshift_return_uint64_from_uint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTUInt64, BitOperandGenericShiftAmount> args(row, parm, *this);
        return (int64_t) args.shiftRight(isNull);
    }
};


// uint64 = sint64 << generic_shift_amount
class Func_rightshift_return_uint64_from_sint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTSInt64, BitOperandGenericShiftAmount> args(row, parm, *this);
        return (int64_t) args.shiftRight(isNull);
    }
};


bool Func_rightshift::fix(execplan::FunctionColumn &col) const
{
    static Func_rightshift_return_uint64_from_uint64 return_uint64_from_uint64;
    static Func_rightshift_return_uint64_from_sint64 return_uint64_from_sint64;
    return fixForBitOp1(col, return_uint64_from_uint64,
                             return_uint64_from_sint64);
}


//
// BIT OR
//


// uint64 = generic | generic
int64_t Func_bitor::getIntVal(Row& row,
                              FunctionParm& parm,
                              bool& isNull,
                              CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 2);
    PairLazy<BitOperandGeneric, BitOperandGeneric> args(row, parm, *this);
    return (int64_t) args.bitOr(isNull);
}


uint64_t Func_bitor::getUintVal(rowgroup::Row& row,
                                FunctionParm& fp,
                                bool& isNull,
                                execplan::CalpontSystemCatalog::ColType& op_ct)
{
    return static_cast<uint64_t>(getIntVal(row, fp, isNull, op_ct));
}


// uint64 = uint64 | uint64
class Func_bitor_return_uint64_from_uint64_uint64: public Func_bitor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairLazy<BitOperandTUInt64, BitOperandTUInt64> args(row, parm, *this);
        return (int64_t) args.bitOr(isNull);
    }
};


// uint64 = sint64 | sint64
class Func_bitor_return_uint64_from_sint64_sint64: public Func_bitor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairLazy<BitOperandTSInt64, BitOperandTSInt64> args(row, parm, *this);
        return (int64_t) args.bitOr(isNull);
    }
};


bool Func_bitor::fix(execplan::FunctionColumn &col) const
{
    static Func_bitor_return_uint64_from_uint64_uint64 return_uint64_from_uint64_uint64;
    static Func_bitor_return_uint64_from_sint64_sint64 return_uint64_from_sint64_sint64;
    return fixForBitOp2(col, return_uint64_from_uint64_uint64,
                             return_uint64_from_sint64_sint64);
}


//
// BIT XOR
//


// uint64 = generic ^ generic
int64_t Func_bitxor::getIntVal(Row& row,
                               FunctionParm& parm,
                               bool& isNull,
                               CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 2);
    PairEager<BitOperandGeneric, BitOperandGeneric> args(row, parm, *this);
    return (int64_t) args.bitXor(isNull);
}


// uint64 = uint64 ^ uint64
class Func_bitxor_return_uint64_from_uint64_uint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTUInt64, BitOperandTUInt64> args(row, parm, *this);
        return (int64_t) args.bitXor(isNull);
    }
};


// uint64 = sint64 ^ sint64
class Func_bitxor_return_uint64_from_sint64_sint64: public Func_bitxor
{
public:
    int64_t getIntVal(Row& row,
                      FunctionParm& parm,
                      bool& isNull,
                      CalpontSystemCatalog::ColType& operationColType)
    {
        idbassert(parm.size() == 2);
        PairEager<BitOperandTSInt64, BitOperandTSInt64> args(row, parm, *this);
        return (int64_t) args.bitXor(isNull);
    }
};


bool Func_bitxor::fix(execplan::FunctionColumn &col) const
{
    static Func_bitxor_return_uint64_from_uint64_uint64 return_uint64_from_uint64_uint64;
    static Func_bitxor_return_uint64_from_sint64_sint64 return_uint64_from_sint64_sint64;
    return fixForBitOp2(col, return_uint64_from_uint64_uint64,
                             return_uint64_from_sint64_sint64);
}


//
// BIT COUNT
//


inline int64_t bitCount(uint64_t val)
{
    // Refer to Hacker's Delight Chapter 5
    // for the bit counting algo used here
    val = val - ((val >> 1) & 0x5555555555555555);
    val = (val & 0x3333333333333333) + ((val >> 2) & 0x3333333333333333);
    val = (val + (val >> 4)) & 0x0F0F0F0F0F0F0F0F;
    val = val + (val >> 8);
    val = val + (val >> 16);
    val = val + (val >> 32);

    return (int64_t)(val & 0x000000000000007F);
}

int64_t Func_bit_count::getIntVal(Row& row,
                                  FunctionParm& parm,
                                  bool& isNull,
                                  CalpontSystemCatalog::ColType& operationColType)
{
    idbassert(parm.size() == 1);
    BitOperandGeneric a(row, parm[0], *this);
    return (isNull= a.isNull()) ? 0 : bitCount((uint64_t) a);
}


} // namespace funcexp
// vim:ts=4 sw=4:
