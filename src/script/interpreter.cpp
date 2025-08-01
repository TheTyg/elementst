// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/interpreter.h>

#include <consensus/consensus.h>
#include <crypto/ripemd160.h>
#include <crypto/sha1.h>
#include <crypto/sha256.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>
extern "C" {
#include <simplicity/elements/exec.h>
#include <simplicity/errorCodes.h>
}

typedef std::vector<unsigned char> valtype;

// These asserts are consensus critical for elements tapscript arithmetic opcodes
static_assert(static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) == UINT64_C(0x7FFFFFFFFFFFFFFF));
static_assert(static_cast<uint64_t>(std::numeric_limits<int64_t>::min()) == UINT64_C(0x8000000000000000));

namespace {

inline bool set_success(ScriptError* ret)
{
    if (ret)
        *ret = SCRIPT_ERR_OK;
    return true;
}

inline bool set_error(ScriptError* ret, const ScriptError serror)
{
    if (ret)
        *ret = serror;
    return false;
}

} // namespace

bool CastToBool(const valtype& vch)
{
    for (unsigned int i = 0; i < vch.size(); i++)
    {
        if (vch[i] != 0)
        {
            // Can be negative zero
            if (i == vch.size()-1 && vch[i] == 0x80)
                return false;
            return true;
        }
    }
    return false;
}

/**
 * Script is a stack machine (like Forth) that evaluates a predicate
 * returning a bool indicating valid or not.  There are no loops.
 */
#define stacktop(i)  (stack.at(stack.size()+(i)))
#define altstacktop(i)  (altstack.at(altstack.size()+(i)))
static inline void popstack(std::vector<valtype>& stack)
{
    if (stack.empty())
        throw std::runtime_error("popstack(): stack empty");
    stack.pop_back();
}

static inline int64_t cast_signed64(uint64_t v)
{
    uint64_t int64_min = static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
    if (v >= int64_min)
        return static_cast<int64_t>(v - int64_min) + std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(v);
}

static inline int64_t read_le8_signed(const unsigned char* ptr)
{
    return cast_signed64(ReadLE64(ptr));
}

static inline void push4_le(std::vector<valtype>& stack, uint32_t v)
{
    uint32_t v_le = htole32(v);
    stack.emplace_back(reinterpret_cast<unsigned char*>(&v_le), reinterpret_cast<unsigned char*>(&v_le) + sizeof(v_le));
}

static inline void push8_le(std::vector<valtype>& stack, uint64_t v)
{
    uint64_t v_le = htole64(v);
    stack.emplace_back(reinterpret_cast<unsigned char*>(&v_le), reinterpret_cast<unsigned char*>(&v_le) + sizeof(v_le));
}

static inline void pushasset(std::vector<valtype>& stack, const CConfidentialAsset& asset)
{
    assert(!asset.IsNull());
    stack.emplace_back(asset.vchCommitment.begin() + 1, asset.vchCommitment.end()); // Push asset without prefix
    stack.emplace_back(asset.vchCommitment.begin(), asset.vchCommitment.begin() + 1); // Push prefix
}

static inline void pushvalue(std::vector<valtype>& stack, const CConfidentialValue& value)
{
    valtype vchinpValue, vchValuePref;
    if (value.IsNull()) {
        // If value is null, explicitly push the explicit prefix 0x01
        vchValuePref = {0x01};
        vchinpValue.assign(8, 0x00);
    } else if (value.IsExplicit()) {
        // Convert BE to LE by using reverse iterator
        vchValuePref.assign(value.vchCommitment.begin(), value.vchCommitment.begin() + 1);
        vchinpValue.assign(value.vchCommitment.rbegin(), value.vchCommitment.rbegin() + 8);
    } else { // (value.IsCommitment())
        vchValuePref.assign(value.vchCommitment.begin(), value.vchCommitment.begin() + 1);
        vchinpValue.assign(value.vchCommitment.begin() + 1, value.vchCommitment.end());
    }
    stack.push_back(std::move(vchinpValue)); // if value is null, 0(LE 8) is pushed
    stack.push_back(std::move(vchValuePref)); // always push prefix
}

static inline void pushspk(std::vector<valtype>& stack, const CScript& scriptPubKey, const uint256& scriptPubKey_sha)
{
    int witnessversion;
    valtype witnessprogram;
    if (scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
        stack.push_back(std::move(witnessprogram));
        stack.push_back(CScriptNum(witnessversion).getvch());
    } else {
        stack.emplace_back(scriptPubKey_sha.begin(), scriptPubKey_sha.end());
        stack.push_back(CScriptNum(-1).getvch());
    }
}

/** Compute the outpoint flag(u8) for a given txin **/
template <class T>
inline unsigned char GetOutpointFlag(const T& txin)
{
    return static_cast<unsigned char> ((!txin.assetIssuance.IsNull() ? (COutPoint::OUTPOINT_ISSUANCE_FLAG >> 24) : 0) |
            (txin.m_is_pegin ? (COutPoint::OUTPOINT_PEGIN_FLAG >> 24) : 0));
}

bool static IsCompressedOrUncompressedPubKey(const valtype &vchPubKey) {
    if (vchPubKey.size() < CPubKey::COMPRESSED_SIZE) {
        //  Non-canonical public key: too short
        return false;
    }
    if (vchPubKey[0] == 0x04) {
        if (vchPubKey.size() != CPubKey::SIZE) {
            //  Non-canonical public key: invalid length for uncompressed key
            return false;
        }
    } else if (vchPubKey[0] == 0x02 || vchPubKey[0] == 0x03) {
        if (vchPubKey.size() != CPubKey::COMPRESSED_SIZE) {
            //  Non-canonical public key: invalid length for compressed key
            return false;
        }
    } else {
        //  Non-canonical public key: neither compressed nor uncompressed
        return false;
    }
    return true;
}

bool static IsCompressedPubKey(const valtype &vchPubKey) {
    if (vchPubKey.size() != CPubKey::COMPRESSED_SIZE) {
        //  Non-canonical public key: invalid length for compressed key
        return false;
    }
    if (vchPubKey[0] != 0x02 && vchPubKey[0] != 0x03) {
        //  Non-canonical public key: invalid prefix for compressed key
        return false;
    }
    return true;
}

/**
 * A canonical signature exists of: <30> <total len> <02> <len R> <R> <02> <len S> <S> <hashtype>
 * Where R and S are not negative (their first byte has its highest bit not set), and not
 * excessively padded (do not start with a 0 byte, unless an otherwise negative number follows,
 * in which case a single 0 byte is necessary and even required).
 *
 * See https://bitcointalk.org/index.php?topic=8392.msg127623#msg127623
 *
 * This function is consensus-critical since BIP66.
 */
bool static IsValidSignatureEncoding(const std::vector<unsigned char> &sig) {
    // Format: 0x30 [total-length] 0x02 [R-length] [R] 0x02 [S-length] [S] [sighash]
    // * total-length: 1-byte length descriptor of everything that follows,
    //   excluding the sighash byte.
    // * R-length: 1-byte length descriptor of the R value that follows.
    // * R: arbitrary-length big-endian encoded R value. It must use the shortest
    //   possible encoding for a positive integer (which means no null bytes at
    //   the start, except a single one when the next byte has its highest bit set).
    // * S-length: 1-byte length descriptor of the S value that follows.
    // * S: arbitrary-length big-endian encoded S value. The same rules apply.
    // * sighash: 1-byte value indicating what data is hashed (not part of the DER
    //   signature)

    // Minimum and maximum size constraints.
    if (sig.size() < 9) return false;
    if (sig.size() > 73) return false;

    // A signature is of type 0x30 (compound).
    if (sig[0] != 0x30) return false;

    // Make sure the length covers the entire signature.
    if (sig[1] != sig.size() - 3) return false;

    // Extract the length of the R element.
    unsigned int lenR = sig[3];

    // Make sure the length of the S element is still inside the signature.
    if (5 + lenR >= sig.size()) return false;

    // Extract the length of the S element.
    unsigned int lenS = sig[5 + lenR];

    // Verify that the length of the signature matches the sum of the length
    // of the elements.
    if ((size_t)(lenR + lenS + 7) != sig.size()) return false;

    // Check whether the R element is an integer.
    if (sig[2] != 0x02) return false;

    // Zero-length integers are not allowed for R.
    if (lenR == 0) return false;

    // Negative numbers are not allowed for R.
    if (sig[4] & 0x80) return false;

    // Null bytes at the start of R are not allowed, unless R would
    // otherwise be interpreted as a negative number.
    if (lenR > 1 && (sig[4] == 0x00) && !(sig[5] & 0x80)) return false;

    // Check whether the S element is an integer.
    if (sig[lenR + 4] != 0x02) return false;

    // Zero-length integers are not allowed for S.
    if (lenS == 0) return false;

    // Negative numbers are not allowed for S.
    if (sig[lenR + 6] & 0x80) return false;

    // Null bytes at the start of S are not allowed, unless S would otherwise be
    // interpreted as a negative number.
    if (lenS > 1 && (sig[lenR + 6] == 0x00) && !(sig[lenR + 7] & 0x80)) return false;

    return true;
}

bool static IsLowDERSignature(const valtype &vchSig, ScriptError* serror) {
    if (!IsValidSignatureEncoding(vchSig)) {
        return set_error(serror, SCRIPT_ERR_SIG_DER);
    }
    // https://bitcoin.stackexchange.com/a/12556:
    //     Also note that inside transaction signatures, an extra hashtype byte
    //     follows the actual signature data.
    std::vector<unsigned char> vchSigCopy(vchSig.begin(), vchSig.begin() + vchSig.size() - 1);
    // If the S value is above the order of the curve divided by two, its
    // complement modulo the order could have been used instead, which is
    // one byte shorter when encoded correctly.
    if (!CPubKey::CheckLowS(vchSigCopy)) {
        return set_error(serror, SCRIPT_ERR_SIG_HIGH_S);
    }
    return true;
}

bool static IsDefinedHashtypeSignature(const valtype &vchSig, unsigned int flags) {
    if (vchSig.size() == 0) {
        return false;
    }
    unsigned char nHashType = vchSig[vchSig.size() - 1] & (~(SIGHASH_ANYONECANPAY));

    // ELEMENTS: Only allow SIGHASH_RANGEPROOF if the flag is set (after dynafed activation).
    if ((flags & SCRIPT_SIGHASH_RANGEPROOF) == SCRIPT_SIGHASH_RANGEPROOF) {
        nHashType = nHashType & (~(SIGHASH_RANGEPROOF));
    }

    if (nHashType < SIGHASH_ALL || nHashType > SIGHASH_SINGLE)
        return false;

    return true;
}

bool CheckSignatureEncoding(const std::vector<unsigned char> &vchSig, unsigned int flags, ScriptError* serror) {
    // Empty signature. Not strictly DER encoded, but allowed to provide a
    // compact way to provide an invalid signature for use with CHECK(MULTI)SIG
    if (vchSig.size() == 0) {
        return true;
    }

    bool no_hash_byte = (flags & SCRIPT_NO_SIGHASH_BYTE) == SCRIPT_NO_SIGHASH_BYTE;
    std::vector<unsigned char> vchSigCopy(vchSig.begin(), vchSig.begin() + vchSig.size());
    // Push a dummy sighash byte to pass checks
    if (no_hash_byte) {
        vchSigCopy.push_back(SIGHASH_ALL);
    }

    if ((flags & (SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_STRICTENC)) != 0 && !IsValidSignatureEncoding(vchSigCopy)) {
        return set_error(serror, SCRIPT_ERR_SIG_DER);
    } else if ((flags & SCRIPT_VERIFY_LOW_S) != 0 && !IsLowDERSignature(vchSigCopy, serror)) {
        // serror is set
        return false;
    } else if ((flags & SCRIPT_VERIFY_STRICTENC) != 0 && !IsDefinedHashtypeSignature(vchSigCopy, flags)) {
        return set_error(serror, SCRIPT_ERR_SIG_HASHTYPE);
    }
    return true;
}

bool static CheckPubKeyEncoding(const valtype &vchPubKey, unsigned int flags, const SigVersion &sigversion, ScriptError* serror) {
    if ((flags & SCRIPT_VERIFY_STRICTENC) != 0 && !IsCompressedOrUncompressedPubKey(vchPubKey)) {
        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);
    }
    // Only compressed keys are accepted in segwit
    if ((flags & SCRIPT_VERIFY_WITNESS_PUBKEYTYPE) != 0 && sigversion == SigVersion::WITNESS_V0 && !IsCompressedPubKey(vchPubKey)) {
        return set_error(serror, SCRIPT_ERR_WITNESS_PUBKEYTYPE);
    }
    return true;
}

bool CheckMinimalPush(const valtype& data, opcodetype opcode) {
    // Excludes OP_1NEGATE, OP_1-16 since they are by definition minimal
    assert(0 <= opcode && opcode <= OP_PUSHDATA4);
    if (data.size() == 0) {
        // Should have used OP_0.
        return opcode == OP_0;
    } else if (data.size() == 1 && data[0] >= 1 && data[0] <= 16) {
        // Should have used OP_1 .. OP_16.
        return false;
    } else if (data.size() == 1 && data[0] == 0x81) {
        // Should have used OP_1NEGATE.
        return false;
    } else if (data.size() <= 75) {
        // Must have used a direct push (opcode indicating number of bytes pushed + those bytes).
        return opcode == data.size();
    } else if (data.size() <= 255) {
        // Must have used OP_PUSHDATA.
        return opcode == OP_PUSHDATA1;
    } else if (data.size() <= 65535) {
        // Must have used OP_PUSHDATA2.
        return opcode == OP_PUSHDATA2;
    }
    return true;
}

int FindAndDelete(CScript& script, const CScript& b)
{
    int nFound = 0;
    if (b.empty())
        return nFound;
    CScript result;
    CScript::const_iterator pc = script.begin(), pc2 = script.begin(), end = script.end();
    opcodetype opcode;
    do
    {
        result.insert(result.end(), pc2, pc);
        while (static_cast<size_t>(end - pc) >= b.size() && std::equal(b.begin(), b.end(), pc))
        {
            pc = pc + b.size();
            ++nFound;
        }
        pc2 = pc;
    }
    while (script.GetOp(pc, opcode));

    if (nFound > 0) {
        result.insert(result.end(), pc2, end);
        script = std::move(result);
    }

    return nFound;
}

namespace {
/** A data type to abstract out the condition stack during script execution.
 *
 * Conceptually it acts like a vector of booleans, one for each level of nested
 * IF/THEN/ELSE, indicating whether we're in the active or inactive branch of
 * each.
 *
 * The elements on the stack cannot be observed individually; we only need to
 * expose whether the stack is empty and whether or not any false values are
 * present at all. To implement OP_ELSE, a toggle_top modifier is added, which
 * flips the last value without returning it.
 *
 * This uses an optimized implementation that does not materialize the
 * actual stack. Instead, it just stores the size of the would-be stack,
 * and the position of the first false value in it.
 */
class ConditionStack {
private:
    //! A constant for m_first_false_pos to indicate there are no falses.
    static constexpr uint32_t NO_FALSE = std::numeric_limits<uint32_t>::max();

    //! The size of the implied stack.
    uint32_t m_stack_size = 0;
    //! The position of the first false value on the implied stack, or NO_FALSE if all true.
    uint32_t m_first_false_pos = NO_FALSE;

public:
    bool empty() const { return m_stack_size == 0; }
    bool all_true() const { return m_first_false_pos == NO_FALSE; }
    void push_back(bool f)
    {
        if (m_first_false_pos == NO_FALSE && !f) {
            // The stack consists of all true values, and a false is added.
            // The first false value will appear at the current size.
            m_first_false_pos = m_stack_size;
        }
        ++m_stack_size;
    }
    void pop_back()
    {
        assert(m_stack_size > 0);
        --m_stack_size;
        if (m_first_false_pos == m_stack_size) {
            // When popping off the first false value, everything becomes true.
            m_first_false_pos = NO_FALSE;
        }
    }
    void toggle_top()
    {
        assert(m_stack_size > 0);
        if (m_first_false_pos == NO_FALSE) {
            // The current stack is all true values; the first false will be the top.
            m_first_false_pos = m_stack_size - 1;
        } else if (m_first_false_pos == m_stack_size - 1) {
            // The top is the first false value; toggling it will make everything true.
            m_first_false_pos = NO_FALSE;
        } else {
            // There is a false value, but not on top. No action is needed as toggling
            // anything but the first false value is unobservable.
        }
    }
};
}

// Check the script has sufficient sigops budget for checksig(crypto) operation
inline bool update_validation_weight(ScriptExecutionData& execdata, ScriptError* serror)
{
    assert(execdata.m_validation_weight_left_init);
    execdata.m_validation_weight_left -= VALIDATION_WEIGHT_PER_SIGOP_PASSED;
    if (execdata.m_validation_weight_left < 0) {
        return set_error(serror, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT);
    }
    return true;
}

static bool EvalChecksigPreTapscript(const valtype& vchSig, const valtype& vchPubKey, CScript::const_iterator pbegincodehash, CScript::const_iterator pend, unsigned int flags, const BaseSignatureChecker& checker, SigVersion sigversion, ScriptError* serror, bool& fSuccess)
{
    assert(sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0);

    // Subset of script starting at the most recent codeseparator
    CScript scriptCode(pbegincodehash, pend);

    // Drop the signature in pre-segwit scripts but not segwit scripts
    if (sigversion == SigVersion::BASE) {
        int found = FindAndDelete(scriptCode, CScript() << vchSig);
        if (found > 0 && (flags & SCRIPT_VERIFY_CONST_SCRIPTCODE))
            return set_error(serror, SCRIPT_ERR_SIG_FINDANDDELETE);
    }

    if (!CheckSignatureEncoding(vchSig, flags, serror) || !CheckPubKeyEncoding(vchPubKey, flags, sigversion, serror)) {
        //serror is set
        return false;
    }
    fSuccess = checker.CheckECDSASignature(vchSig, vchPubKey, scriptCode, sigversion, flags);

    if (!fSuccess && (flags & SCRIPT_VERIFY_NULLFAIL) && vchSig.size())
        return set_error(serror, SCRIPT_ERR_SIG_NULLFAIL);

    return true;
}

static bool EvalTapScriptCheckSigFromStack(const valtype& sig, const valtype& vchPubKey, ScriptExecutionData& execdata, unsigned int flags, const valtype& msg, SigVersion sigversion, ScriptError* serror, bool& success)
{
    // This code follows the behaviour of EvalCheckSigTapscript
    assert(sigversion == SigVersion::TAPSCRIPT);

    /*
     *  The following validation sequence is consensus critical. Please note how --
     *    upgradable public key versions precede other rules;
     *    the script execution fails when using empty signature with invalid public key;
     *    the script execution fails when using non-empty invalid signature.
     */
    success = !sig.empty();
    if (success) {
        // Implement the sigops/witnesssize ratio test.
        // Passing with an upgradable public key version is also counted.
        if (!update_validation_weight(execdata, serror)) return false; // serror is set
    }
    if (vchPubKey.size() == 0) {
        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);
    } else if (vchPubKey.size() == 32) {
        if (success) {
            if (sig.size() != 64)
                return set_error(serror, SCRIPT_ERR_SCHNORR_SIG_SIZE);
            const XOnlyPubKey pubkey{vchPubKey};
            if (!pubkey.VerifySchnorr(msg, sig))
                return set_error(serror, SCRIPT_ERR_SCHNORR_SIG);
        }
    } else {
        /*
         *  New public key version softforks should be defined before this `else` block.
         *  Generally, the new code should not do anything but failing the script execution. To avoid
         *  consensus bugs, it should not modify any existing values (including `success`).
         */
        if ((flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_PUBKEYTYPE) != 0) {
            return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_PUBKEYTYPE);
        }
    }

    return true;
}

static bool EvalChecksigTapscript(const valtype& sig, const valtype& pubkey, ScriptExecutionData& execdata, unsigned int flags, const BaseSignatureChecker& checker, SigVersion sigversion, ScriptError* serror, bool& success)
{
    assert(sigversion == SigVersion::TAPSCRIPT);

    /*
     *  The following validation sequence is consensus critical. Please note how --
     *    upgradable public key versions precede other rules;
     *    the script execution fails when using empty signature with invalid public key;
     *    the script execution fails when using non-empty invalid signature.
     */
    success = !sig.empty();
    if (success) {
        // Implement the sigops/witnesssize ratio test.
        // Passing with an upgradable public key version is also counted.
        if (!update_validation_weight(execdata, serror)) return false; // serror is set
    }
    if (pubkey.size() == 0) {
        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);
    } else if (pubkey.size() == 32) {
        if (success && !checker.CheckSchnorrSignature(sig, pubkey, sigversion, execdata, serror)) {
            return false; // serror is set
        }
    } else {
        /*
         *  New public key version softforks should be defined before this `else` block.
         *  Generally, the new code should not do anything but failing the script execution. To avoid
         *  consensus bugs, it should not modify any existing values (including `success`).
         */
        if ((flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_PUBKEYTYPE) != 0) {
            return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_PUBKEYTYPE);
        }
    }

    return true;
}

/** Helper for OP_CHECKSIG, OP_CHECKSIGVERIFY, and (in Tapscript) OP_CHECKSIGADD.
 *
 * A return value of false means the script fails entirely. When true is returned, the
 * success variable indicates whether the signature check itself succeeded.
 */
static bool EvalChecksig(const valtype& sig, const valtype& pubkey, CScript::const_iterator pbegincodehash, CScript::const_iterator pend, ScriptExecutionData& execdata, unsigned int flags, const BaseSignatureChecker& checker, SigVersion sigversion, ScriptError* serror, bool& success)
{
    switch (sigversion) {
    case SigVersion::BASE:
    case SigVersion::WITNESS_V0:
        return EvalChecksigPreTapscript(sig, pubkey, pbegincodehash, pend, flags, checker, sigversion, serror, success);
    case SigVersion::TAPSCRIPT:
        return EvalChecksigTapscript(sig, pubkey, execdata, flags, checker, sigversion, serror, success);
    case SigVersion::TAPROOT:
        // Key path spending in Taproot has no script, so this is unreachable.
        break;
    }
    assert(false);
}

const CHashWriter HASHER_TAPLEAF_ELEMENTS = TaggedHash("TapLeaf/elements");
const CHashWriter HASHER_TAPBRANCH_ELEMENTS = TaggedHash("TapBranch/elements");
const CHashWriter HASHER_TAPSIGHASH_ELEMENTS = TaggedHash("TapSighash/elements");

bool EvalScript(std::vector<std::vector<unsigned char> >& stack, const CScript& script, unsigned int flags, const BaseSignatureChecker& checker, SigVersion sigversion, ScriptExecutionData& execdata, ScriptError* serror)
{
    static const CScriptNum bnZero(0);
    static const CScriptNum bnOne(1);
    // static const CScriptNum bnFalse(0);
    // static const CScriptNum bnTrue(1);
    static const valtype vchFalse(0);
    static const valtype vchZero(0);
    static const valtype vchTrue(1, 1);

    // sigversion cannot be TAPROOT here, as it admits no script execution.
    assert(sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0 || sigversion == SigVersion::TAPSCRIPT);

    CScript::const_iterator pc = script.begin();
    CScript::const_iterator pend = script.end();
    CScript::const_iterator pbegincodehash = script.begin();
    opcodetype opcode;
    valtype vchPushValue;
    ConditionStack vfExec;
    std::vector<valtype> altstack;
    set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);
    if ((sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) && script.size() > MAX_SCRIPT_SIZE) {
        return set_error(serror, SCRIPT_ERR_SCRIPT_SIZE);
    }
    int nOpCount = 0;
    bool fRequireMinimal = (flags & SCRIPT_VERIFY_MINIMALDATA) != 0;
    uint32_t opcode_pos = 0;
    execdata.m_codeseparator_pos = 0xFFFFFFFFUL;
    execdata.m_codeseparator_pos_init = true;

    try
    {
        for (; pc < pend; ++opcode_pos) {
            bool fExec = vfExec.all_true();

            //
            // Read instruction
            //
            if (!script.GetOp(pc, opcode, vchPushValue))
                return set_error(serror, SCRIPT_ERR_BAD_OPCODE);
            if (vchPushValue.size() > MAX_SCRIPT_ELEMENT_SIZE)
                return set_error(serror, SCRIPT_ERR_PUSH_SIZE);

            if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) {
                // Note how OP_RESERVED does not count towards the opcode limit.
                if (opcode > OP_16 && ++nOpCount > MAX_OPS_PER_SCRIPT) {
                    return set_error(serror, SCRIPT_ERR_OP_COUNT);
                }
            }

            // ELEMENTS:
            // commented out opcodes are re-enabled in Elements
            if (//opcode == OP_CAT ||
                //opcode == OP_SUBSTR ||
                //opcode == OP_LEFT ||
                //opcode == OP_RIGHT ||
                //opcode == OP_INVERT ||
                //opcode == OP_AND ||
                //opcode == OP_OR ||
                //opcode == OP_XOR ||
                //opcode == OP_LSHIFT ||
                //opcode == OP_RSHIFT ||
                opcode == OP_2MUL ||
                opcode == OP_2DIV ||
                opcode == OP_MUL ||
                opcode == OP_DIV ||
                opcode == OP_MOD
            ) {
                return set_error(serror, SCRIPT_ERR_DISABLED_OPCODE); // Disabled opcodes (CVE-2010-5137).
            }

            // With SCRIPT_VERIFY_CONST_SCRIPTCODE, OP_CODESEPARATOR in non-segwit script is rejected even in an unexecuted branch
            if (opcode == OP_CODESEPARATOR && sigversion == SigVersion::BASE && (flags & SCRIPT_VERIFY_CONST_SCRIPTCODE))
                return set_error(serror, SCRIPT_ERR_OP_CODESEPARATOR);

            if (fExec && 0 <= opcode && opcode <= OP_PUSHDATA4) {
                if (fRequireMinimal && !CheckMinimalPush(vchPushValue, opcode)) {
                    return set_error(serror, SCRIPT_ERR_MINIMALDATA);
                }
                stack.push_back(vchPushValue);
            } else if (fExec || (OP_IF <= opcode && opcode <= OP_ENDIF))
            switch (opcode)
            {
                //
                // Push value
                //
                case OP_1NEGATE:
                case OP_1:
                case OP_2:
                case OP_3:
                case OP_4:
                case OP_5:
                case OP_6:
                case OP_7:
                case OP_8:
                case OP_9:
                case OP_10:
                case OP_11:
                case OP_12:
                case OP_13:
                case OP_14:
                case OP_15:
                case OP_16:
                {
                    // ( -- value)
                    CScriptNum bn((int)opcode - (int)(OP_1 - 1));
                    stack.push_back(bn.getvch());
                    // The result of these opcodes should always be the minimal way to push the data
                    // they push, so no need for a CheckMinimalPush here.
                }
                break;


                //
                // Control
                //
                case OP_NOP:
                    break;

                case OP_CHECKLOCKTIMEVERIFY:
                {
                    if (!(flags & SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY)) {
                        // not enabled; treat as a NOP2
                        break;
                    }

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    // Note that elsewhere numeric opcodes are limited to
                    // operands in the range -2**31+1 to 2**31-1, however it is
                    // legal for opcodes to produce results exceeding that
                    // range. This limitation is implemented by CScriptNum's
                    // default 4-byte limit.
                    //
                    // If we kept to that limit we'd have a year 2038 problem,
                    // even though the nLockTime field in transactions
                    // themselves is uint32 which only becomes meaningless
                    // after the year 2106.
                    //
                    // Thus as a special case we tell CScriptNum to accept up
                    // to 5-byte bignums, which are good until 2**39-1, well
                    // beyond the 2**32-1 limit of the nLockTime field itself.
                    const CScriptNum nLockTime(stacktop(-1), fRequireMinimal, 5);

                    // In the rare event that the argument may be < 0 due to
                    // some arithmetic being done first, you can always use
                    // 0 MAX CHECKLOCKTIMEVERIFY.
                    if (nLockTime < 0)
                        return set_error(serror, SCRIPT_ERR_NEGATIVE_LOCKTIME);

                    // Actually compare the specified lock time with the transaction.
                    if (!checker.CheckLockTime(nLockTime))
                        return set_error(serror, SCRIPT_ERR_UNSATISFIED_LOCKTIME);

                    break;
                }

                case OP_CHECKSEQUENCEVERIFY:
                {
                    if (!(flags & SCRIPT_VERIFY_CHECKSEQUENCEVERIFY)) {
                        // not enabled; treat as a NOP3
                        break;
                    }

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    // nSequence, like nLockTime, is a 32-bit unsigned integer
                    // field. See the comment in CHECKLOCKTIMEVERIFY regarding
                    // 5-byte numeric operands.
                    const CScriptNum nSequence(stacktop(-1), fRequireMinimal, 5);

                    // In the rare event that the argument may be < 0 due to
                    // some arithmetic being done first, you can always use
                    // 0 MAX CHECKSEQUENCEVERIFY.
                    if (nSequence < 0)
                        return set_error(serror, SCRIPT_ERR_NEGATIVE_LOCKTIME);

                    // To provide for future soft-fork extensibility, if the
                    // operand has the disabled lock-time flag set,
                    // CHECKSEQUENCEVERIFY behaves as a NOP.
                    if ((nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) != 0)
                        break;

                    // Compare the specified sequence number with the input.
                    if (!checker.CheckSequence(nSequence))
                        return set_error(serror, SCRIPT_ERR_UNSATISFIED_LOCKTIME);

                    break;
                }

                case OP_NOP1: case OP_NOP4: case OP_NOP5:
                case OP_NOP6: case OP_NOP7: case OP_NOP8: case OP_NOP9: case OP_NOP10:
                {
                    if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_NOPS)
                        return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_NOPS);
                }
                break;

                case OP_IF:
                case OP_NOTIF:
                {
                    // <expression> if [statements] [else [statements]] endif
                    bool fValue = false;
                    if (fExec)
                    {
                        if (stack.size() < 1)
                            return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                        valtype& vch = stacktop(-1);
                        // Tapscript requires minimal IF/NOTIF inputs as a consensus rule.
                        if (sigversion == SigVersion::TAPSCRIPT) {
                            // The input argument to the OP_IF and OP_NOTIF opcodes must be either
                            // exactly 0 (the empty vector) or exactly 1 (the one-byte vector with value 1).
                            if (vch.size() > 1 || (vch.size() == 1 && vch[0] != 1)) {
                                return set_error(serror, SCRIPT_ERR_TAPSCRIPT_MINIMALIF);
                            }
                        }
                        // Under witness v0 rules it is only a policy rule, enabled through SCRIPT_VERIFY_MINIMALIF.
                        if (sigversion == SigVersion::WITNESS_V0 && (flags & SCRIPT_VERIFY_MINIMALIF)) {
                            if (vch.size() > 1)
                                return set_error(serror, SCRIPT_ERR_MINIMALIF);
                            if (vch.size() == 1 && vch[0] != 1)
                                return set_error(serror, SCRIPT_ERR_MINIMALIF);
                        }
                        fValue = CastToBool(vch);
                        if (opcode == OP_NOTIF)
                            fValue = !fValue;
                        popstack(stack);
                    }
                    vfExec.push_back(fValue);
                }
                break;

                case OP_ELSE:
                {
                    if (vfExec.empty())
                        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                    vfExec.toggle_top();
                }
                break;

                case OP_ENDIF:
                {
                    if (vfExec.empty())
                        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);
                    vfExec.pop_back();
                }
                break;

                case OP_VERIFY:
                {
                    // (true -- ) or
                    // (false -- false) and return
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    bool fValue = CastToBool(stacktop(-1));
                    if (fValue)
                        popstack(stack);
                    else
                        return set_error(serror, SCRIPT_ERR_VERIFY);
                }
                break;

                case OP_RETURN:
                {
                    return set_error(serror, SCRIPT_ERR_OP_RETURN);
                }
                break;


                //
                // Stack ops
                //
                case OP_TOALTSTACK:
                {
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    altstack.push_back(stacktop(-1));
                    popstack(stack);
                }
                break;

                case OP_FROMALTSTACK:
                {
                    if (altstack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_ALTSTACK_OPERATION);
                    stack.push_back(altstacktop(-1));
                    popstack(altstack);
                }
                break;

                case OP_2DROP:
                {
                    // (x1 x2 -- )
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    popstack(stack);
                    popstack(stack);
                }
                break;

                case OP_2DUP:
                {
                    // (x1 x2 -- x1 x2 x1 x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-2);
                    valtype vch2 = stacktop(-1);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_3DUP:
                {
                    // (x1 x2 x3 -- x1 x2 x3 x1 x2 x3)
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-3);
                    valtype vch2 = stacktop(-2);
                    valtype vch3 = stacktop(-1);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                    stack.push_back(vch3);
                }
                break;

                case OP_2OVER:
                {
                    // (x1 x2 x3 x4 -- x1 x2 x3 x4 x1 x2)
                    if (stack.size() < 4)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-4);
                    valtype vch2 = stacktop(-3);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_2ROT:
                {
                    // (x1 x2 x3 x4 x5 x6 -- x3 x4 x5 x6 x1 x2)
                    if (stack.size() < 6)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-6);
                    valtype vch2 = stacktop(-5);
                    stack.erase(stack.end()-6, stack.end()-4);
                    stack.push_back(vch1);
                    stack.push_back(vch2);
                }
                break;

                case OP_2SWAP:
                {
                    // (x1 x2 x3 x4 -- x3 x4 x1 x2)
                    if (stack.size() < 4)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-4), stacktop(-2));
                    swap(stacktop(-3), stacktop(-1));
                }
                break;

                case OP_IFDUP:
                {
                    // (x - 0 | x x)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    if (CastToBool(vch))
                        stack.push_back(vch);
                }
                break;

                case OP_DEPTH:
                {
                    // -- stacksize
                    CScriptNum bn(stack.size());
                    stack.push_back(bn.getvch());
                }
                break;

                case OP_DROP:
                {
                    // (x -- )
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    popstack(stack);
                }
                break;

                case OP_DUP:
                {
                    // (x -- x x)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    stack.push_back(vch);
                }
                break;

                case OP_NIP:
                {
                    // (x1 x2 -- x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    stack.erase(stack.end() - 2);
                }
                break;

                case OP_OVER:
                {
                    // (x1 x2 -- x1 x2 x1)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-2);
                    stack.push_back(vch);
                }
                break;

                case OP_PICK:
                case OP_ROLL:
                {
                    // (xn ... x2 x1 x0 n - xn ... x2 x1 x0 xn)
                    // (xn ... x2 x1 x0 n - ... x2 x1 x0 xn)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    int n = CScriptNum(stacktop(-1), fRequireMinimal).getint();
                    popstack(stack);
                    if (n < 0 || n >= (int)stack.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-n-1);
                    if (opcode == OP_ROLL)
                        stack.erase(stack.end()-n-1);
                    stack.push_back(vch);
                }
                break;

                case OP_ROT:
                {
                    // (x1 x2 x3 -- x2 x3 x1)
                    //  x2 x1 x3  after first swap
                    //  x2 x3 x1  after second swap
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-3), stacktop(-2));
                    swap(stacktop(-2), stacktop(-1));
                }
                break;

                case OP_SWAP:
                {
                    // (x1 x2 -- x2 x1)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    swap(stacktop(-2), stacktop(-1));
                }
                break;

                case OP_TUCK:
                {
                    // (x1 x2 -- x2 x1 x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch = stacktop(-1);
                    stack.insert(stack.end()-2, vch);
                }
                break;

                case OP_CAT:
                {
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-2);
                    valtype vch2 = stacktop(-1);

                    if (vch1.size() + vch2.size() > MAX_SCRIPT_ELEMENT_SIZE)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch3;
                    vch3.reserve(vch1.size() + vch2.size());
                    vch3.insert(vch3.end(), vch1.begin(), vch1.end());
                    vch3.insert(vch3.end(), vch2.begin(), vch2.end());

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch3);
                }
                break;

                case OP_SIZE:
                {
                    // (in -- in size)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn(stacktop(-1).size());
                    stack.push_back(bn.getvch());
                }
                break;


                //
                // String operators
                //
                case OP_LEFT:
                case OP_RIGHT:
                {
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch1 = stacktop(-2);
                    CScriptNum start(stacktop(-1), fRequireMinimal);

                    if (start < 0)
                        return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);

                    valtype vch2;
                    switch (opcode) {
                        case OP_RIGHT:
                        {
                            if (start >= vch1.size())
                                vch2 = vchZero;
                            else
                                vch2.insert(vch2.begin(), vch1.begin() + start.getint(), vch1.end());
                            break;
                        }
                        case OP_LEFT:
                        {
                            if (start >= vch1.size())
                                vch2 = vch1;
                            else
                                vch2.insert(vch2.begin(), vch1.begin(), vch1.begin() + start.getint());
                            break;
                        }
                        default:
                        {
                            assert(!"invalid opcode");
                            break;
                        }
                    }
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch2);
                }
                break;

                case OP_SUBSTR:
                case OP_SUBSTR_LAZY:
                {
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch1 = stacktop(-3);
                    CScriptNum start(stacktop(-2), fRequireMinimal);
                    CScriptNum length(stacktop(-1), fRequireMinimal);

                    if (opcode == OP_SUBSTR_LAZY) {
                        if (start < 0)
                            start = 0;

                        if (length < 0)
                            length = 0;

                        if (start >= vch1.size()) {
                            popstack(stack);
                            popstack(stack);
                            popstack(stack);
                            stack.push_back(vchZero);
                            break;
                        }

                        if (length > MAX_SCRIPT_ELEMENT_SIZE)
                            length = MAX_SCRIPT_ELEMENT_SIZE;

                        // start + length cannot overflow because of the restrictions immediately above
                        if (start + length > vch1.size()) {
                            length = CScriptNum(vch1.size()) - start;
                        }
                    }

                    if (length < 0 || start < 0)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    if (start >= vch1.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    if (length > vch1.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    if ((start + length) > vch1.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch2;
                    vch2.insert(vch2.begin(), vch1.begin() + start.getint(), vch1.begin() + (start + length).getint());

                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch2);
                }
                break;


                //
                // Bitwise logic
                //
                case OP_RSHIFT:
                {
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-2);
                    CScriptNum bn(stacktop(-1), fRequireMinimal);

                    if (bn < 0)
                        return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);

                    unsigned int full_bytes = bn.getint() / 8;
                    unsigned int bits = bn.getint() % 8;

                    if (full_bytes >= vch1.size()) {
                        popstack(stack);
                        popstack(stack);
                        stack.push_back(vchZero);
                        break;
                    }

                    valtype vch2;
                    vch2.insert(vch2.begin(), vch1.begin() + full_bytes, vch1.end());

                    uint16_t temp = 0;
                    for (int i=(vch2.size()-1);i>=0;--i) {
                        temp = (vch2[i] << (8 - bits)) | ((temp << 8) & 0xff00);
                        vch2[i] = (temp & 0xff00) >> 8;
                    }

                    // 0x0fff >> 4 == 0x00ff or 0xff, reduce to minimal representation
                    while (!vch2.empty() && vch2.back() == 0)
                        vch2.pop_back();

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch2);
                }
                break;

                case OP_LSHIFT:
                {
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype vch1 = stacktop(-2);
                    CScriptNum bn(stacktop(-1), fRequireMinimal);

                    if (bn < 0)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    unsigned int full_bytes = bn.getint() / 8;
                    unsigned int bits = bn.getint() % 8;

                    if (vch1.size() + full_bytes + (bits ? 1 : 0) > MAX_SCRIPT_ELEMENT_SIZE)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch2;
                    vch2.reserve(vch1.size() + full_bytes + 1);
                    vch2.insert(vch2.end(), full_bytes, 0);
                    vch2.insert(vch2.end(), vch1.begin(), vch1.end());
                    vch2.insert(vch2.end(), 1, 0);

                    uint16_t temp = 0;
                    for (size_t i=0;i<vch2.size();++i) {
                        temp = (vch2[i] << bits) | (temp >> 8);
                        vch2[i] = temp & 0xff;
                    }

                    // reduce to minimal representation
                    while (!vch2.empty() && vch2.back() == 0)
                        vch2.pop_back();

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch2);
                }
                break;

                case OP_INVERT:
                {
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-1);
                    for (size_t i = 0; i < vch1.size(); ++i)
                        vch1[i] = ~vch1[i];
                }
                break;

                case OP_AND:
                {
                    // (x1 x2 -- x1 & x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-1);
                    valtype& vch2 = stacktop(-2);
                    if (vch1.size() != vch2.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch3(vch1);
                    for (size_t i = 0; i < vch1.size(); i++)
                        vch3[i] &= vch2[i];
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch3);
                }
                break;

                case OP_OR:
                {
                    // (x1 x2 -- x1 | x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-1);
                    valtype& vch2 = stacktop(-2);
                    if (vch1.size() != vch2.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch3(vch1);
                    for (size_t i = 0; i < vch1.size(); i++)
                        vch3[i] |= vch2[i];
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch3);
                }
                break;

                case OP_XOR:
                {
                    // (x1 x2 -- x1 ^ x2)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-1);
                    valtype& vch2 = stacktop(-2);
                    if (vch1.size() != vch2.size())
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vch3(vch1);
                    for (size_t i = 0; i < vch1.size(); i++)
                        vch3[i] ^= vch2[i];
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(vch3);
                }
                break;

                case OP_EQUAL:
                case OP_EQUALVERIFY:
                //case OP_NOTEQUAL: // use OP_NUMNOTEQUAL
                {
                    // (x1 x2 - bool)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch1 = stacktop(-2);
                    valtype& vch2 = stacktop(-1);
                    bool fEqual = (vch1 == vch2);
                    // OP_NOTEQUAL is disabled because it would be too easy to say
                    // something like n != 1 and have some wiseguy pass in 1 with extra
                    // zero bytes after it (numerically, 0x01 == 0x0001 == 0x000001)
                    //if (opcode == OP_NOTEQUAL)
                    //    fEqual = !fEqual;
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fEqual ? vchTrue : vchFalse);
                    if (opcode == OP_EQUALVERIFY)
                    {
                        if (fEqual)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_EQUALVERIFY);
                    }
                }
                break;


                //
                // Numeric
                //
                case OP_1ADD:
                case OP_1SUB:
                case OP_NEGATE:
                case OP_ABS:
                case OP_NOT:
                case OP_0NOTEQUAL:
                {
                    // (in -- out)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn(stacktop(-1), fRequireMinimal);
                    switch (opcode)
                    {
                    case OP_1ADD:       bn += bnOne; break;
                    case OP_1SUB:       bn -= bnOne; break;
                    case OP_NEGATE:     bn = -bn; break;
                    case OP_ABS:        if (bn < bnZero) bn = -bn; break;
                    case OP_NOT:        bn = (bn == bnZero); break;
                    case OP_0NOTEQUAL:  bn = (bn != bnZero); break;
                    default:            assert(!"invalid opcode"); break;
                    }
                    popstack(stack);
                    stack.push_back(bn.getvch());
                }
                break;

                case OP_ADD:
                case OP_SUB:
                case OP_BOOLAND:
                case OP_BOOLOR:
                case OP_NUMEQUAL:
                case OP_NUMEQUALVERIFY:
                case OP_NUMNOTEQUAL:
                case OP_LESSTHAN:
                case OP_GREATERTHAN:
                case OP_LESSTHANOREQUAL:
                case OP_GREATERTHANOREQUAL:
                case OP_MIN:
                case OP_MAX:
                {
                    // (x1 x2 -- out)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn1(stacktop(-2), fRequireMinimal);
                    CScriptNum bn2(stacktop(-1), fRequireMinimal);
                    CScriptNum bn(0);
                    switch (opcode)
                    {
                    case OP_ADD:
                        bn = bn1 + bn2;
                        break;

                    case OP_SUB:
                        bn = bn1 - bn2;
                        break;

                    case OP_BOOLAND:             bn = (bn1 != bnZero && bn2 != bnZero); break;
                    case OP_BOOLOR:              bn = (bn1 != bnZero || bn2 != bnZero); break;
                    case OP_NUMEQUAL:            bn = (bn1 == bn2); break;
                    case OP_NUMEQUALVERIFY:      bn = (bn1 == bn2); break;
                    case OP_NUMNOTEQUAL:         bn = (bn1 != bn2); break;
                    case OP_LESSTHAN:            bn = (bn1 < bn2); break;
                    case OP_GREATERTHAN:         bn = (bn1 > bn2); break;
                    case OP_LESSTHANOREQUAL:     bn = (bn1 <= bn2); break;
                    case OP_GREATERTHANOREQUAL:  bn = (bn1 >= bn2); break;
                    case OP_MIN:                 bn = (bn1 < bn2 ? bn1 : bn2); break;
                    case OP_MAX:                 bn = (bn1 > bn2 ? bn1 : bn2); break;
                    default:                     assert(!"invalid opcode"); break;
                    }
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(bn.getvch());

                    if (opcode == OP_NUMEQUALVERIFY)
                    {
                        if (CastToBool(stacktop(-1)))
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_NUMEQUALVERIFY);
                    }
                }
                break;

                case OP_WITHIN:
                {
                    // (x min max -- out)
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    CScriptNum bn1(stacktop(-3), fRequireMinimal);
                    CScriptNum bn2(stacktop(-2), fRequireMinimal);
                    CScriptNum bn3(stacktop(-1), fRequireMinimal);
                    bool fValue = (bn2 <= bn1 && bn1 < bn3);
                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fValue ? vchTrue : vchFalse);
                }
                break;


                //
                // Crypto
                //
                case OP_RIPEMD160:
                case OP_SHA1:
                case OP_SHA256:
                case OP_HASH160:
                case OP_HASH256:
                {
                    // (in -- hash)
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    valtype& vch = stacktop(-1);
                    valtype vchHash((opcode == OP_RIPEMD160 || opcode == OP_SHA1 || opcode == OP_HASH160) ? 20 : 32);
                    if (opcode == OP_RIPEMD160)
                        CRIPEMD160().Write(vch.data(), vch.size()).Finalize(vchHash.data());
                    else if (opcode == OP_SHA1)
                        CSHA1().Write(vch.data(), vch.size()).Finalize(vchHash.data());
                    else if (opcode == OP_SHA256)
                        CSHA256().Write(vch.data(), vch.size()).Finalize(vchHash.data());
                    else if (opcode == OP_HASH160)
                        CHash160().Write(vch).Finalize(vchHash);
                    else if (opcode == OP_HASH256)
                        CHash256().Write(vch).Finalize(vchHash);
                    popstack(stack);
                    stack.push_back(vchHash);
                }
                break;

                case OP_CODESEPARATOR:
                {
                    // If SCRIPT_VERIFY_CONST_SCRIPTCODE flag is set, use of OP_CODESEPARATOR is rejected in pre-segwit
                    // script, even in an unexecuted branch (this is checked above the opcode case statement).

                    // Hash starts after the code separator
                    pbegincodehash = pc;
                    execdata.m_codeseparator_pos = opcode_pos;
                }
                break;

                case OP_CHECKSIG:
                case OP_CHECKSIGVERIFY:
                {
                    // (sig pubkey -- bool)
                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchSig    = stacktop(-2);
                    valtype& vchPubKey = stacktop(-1);

                    bool fSuccess = true;
                    if (!EvalChecksig(vchSig, vchPubKey, pbegincodehash, pend, execdata, flags, checker, sigversion, serror, fSuccess)) return false;
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fSuccess ? vchTrue : vchFalse);
                    if (opcode == OP_CHECKSIGVERIFY)
                    {
                        if (fSuccess)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_CHECKSIGVERIFY);
                    }
                }
                break;

                case OP_CHECKSIGADD:
                {
                    // OP_CHECKSIGADD is only available in Tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    // (sig num pubkey -- num)
                    if (stack.size() < 3) return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    const valtype& sig = stacktop(-3);
                    const CScriptNum num(stacktop(-2), fRequireMinimal);
                    const valtype& pubkey = stacktop(-1);

                    bool success = true;
                    if (!EvalChecksig(sig, pubkey, pbegincodehash, pend, execdata, flags, checker, sigversion, serror, success)) return false;
                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back((num + (success ? 1 : 0)).getvch());
                }
                break;

                case OP_CHECKMULTISIG:
                case OP_CHECKMULTISIGVERIFY:
                {
                    if (sigversion == SigVersion::TAPSCRIPT) return set_error(serror, SCRIPT_ERR_TAPSCRIPT_CHECKMULTISIG);

                    // ([sig ...] num_of_signatures [pubkey ...] num_of_pubkeys -- bool)

                    int i = 1;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int nKeysCount = CScriptNum(stacktop(-i), fRequireMinimal).getint();
                    if (nKeysCount < 0 || nKeysCount > MAX_PUBKEYS_PER_MULTISIG)
                        return set_error(serror, SCRIPT_ERR_PUBKEY_COUNT);
                    nOpCount += nKeysCount;
                    if (nOpCount > MAX_OPS_PER_SCRIPT)
                        return set_error(serror, SCRIPT_ERR_OP_COUNT);
                    int ikey = ++i;
                    // ikey2 is the position of last non-signature item in the stack. Top stack item = 1.
                    // With SCRIPT_VERIFY_NULLFAIL, this is used for cleanup if operation fails.
                    int ikey2 = nKeysCount + 2;
                    i += nKeysCount;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int nSigsCount = CScriptNum(stacktop(-i), fRequireMinimal).getint();
                    if (nSigsCount < 0 || nSigsCount > nKeysCount)
                        return set_error(serror, SCRIPT_ERR_SIG_COUNT);
                    int isig = ++i;
                    i += nSigsCount;
                    if ((int)stack.size() < i)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    // Subset of script starting at the most recent codeseparator
                    CScript scriptCode(pbegincodehash, pend);

                    // Drop the signature in pre-segwit scripts but not segwit scripts
                    for (int k = 0; k < nSigsCount; k++)
                    {
                        valtype& vchSig = stacktop(-isig-k);
                        if (sigversion == SigVersion::BASE) {
                            int found = FindAndDelete(scriptCode, CScript() << vchSig);
                            if (found > 0 && (flags & SCRIPT_VERIFY_CONST_SCRIPTCODE))
                                return set_error(serror, SCRIPT_ERR_SIG_FINDANDDELETE);
                        }
                    }

                    bool fSuccess = true;
                    while (fSuccess && nSigsCount > 0)
                    {
                        valtype& vchSig    = stacktop(-isig);
                        valtype& vchPubKey = stacktop(-ikey);

                        // Note how this makes the exact order of pubkey/signature evaluation
                        // distinguishable by CHECKMULTISIG NOT if the STRICTENC flag is set.
                        // See the script_(in)valid tests for details.
                        if (!CheckSignatureEncoding(vchSig, flags, serror) || !CheckPubKeyEncoding(vchPubKey, flags, sigversion, serror)) {
                            // serror is set
                            return false;
                        }

                        // Check signature
                        bool fOk = checker.CheckECDSASignature(vchSig, vchPubKey, scriptCode, sigversion, flags);

                        if (fOk) {
                            isig++;
                            nSigsCount--;
                        }
                        ikey++;
                        nKeysCount--;

                        // If there are more signatures left than keys left,
                        // then too many signatures have failed. Exit early,
                        // without checking any further signatures.
                        if (nSigsCount > nKeysCount)
                            fSuccess = false;
                    }

                    // Clean up stack of actual arguments
                    while (i-- > 1) {
                        // If the operation failed, we require that all signatures must be empty vector
                        if (!fSuccess && (flags & SCRIPT_VERIFY_NULLFAIL) && !ikey2 && stacktop(-1).size())
                            return set_error(serror, SCRIPT_ERR_SIG_NULLFAIL);
                        if (ikey2 > 0)
                            ikey2--;
                        popstack(stack);
                    }

                    // A bug causes CHECKMULTISIG to consume one extra argument
                    // whose contents were not checked in any way.
                    //
                    // Unfortunately this is a potential source of mutability,
                    // so optionally verify it is exactly equal to zero prior
                    // to removing it from the stack.
                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);
                    if ((flags & SCRIPT_VERIFY_NULLDUMMY) && stacktop(-1).size())
                        return set_error(serror, SCRIPT_ERR_SIG_NULLDUMMY);
                    popstack(stack);

                    stack.push_back(fSuccess ? vchTrue : vchFalse);

                    if (opcode == OP_CHECKMULTISIGVERIFY)
                    {
                        if (fSuccess)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_CHECKMULTISIGVERIFY);
                    }
                }
                break;

                case OP_DETERMINISTICRANDOM:
                {
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype vchSeed = stacktop(-3);
                    CScriptNum bnMin(stacktop(-2), fRequireMinimal);
                    CScriptNum bnMax(stacktop(-1), fRequireMinimal);

                    if (bnMin > bnMax)
                        return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);

                    if (bnMin == bnMax) {
                        popstack(stack);
                        popstack(stack);
                        popstack(stack);
                        stack.push_back(bnMin.getvch());
                        break;
                    }

                    // The range of the random source must be a multiple of the modulus
                    // to give every possible output value an equal possibility
                    uint64_t nMax = (bnMax-bnMin).getint();
                    uint64_t nRange = (std::numeric_limits<uint64_t>::max() / nMax) * nMax;
                    uint64_t nRand;

                    valtype vchHash(32, 0);
                    uint64_t nCounter = 0;
                    int nHashIndex = 3;
                    CSHA256 hasher;
                    hasher.Write(vchSeed.data(), vchSeed.size());
                    do {
                        if (nHashIndex >= 3) {
                            uint64_t le_counter = htole64(nCounter);
                            CSHA256(hasher).Write((const unsigned char*)&le_counter, sizeof(nCounter)).Finalize(vchHash.data());
                            nHashIndex = 0;
                            nCounter++;
                        }

                        nRand = 0;
                        for (size_t i=0; i<8; ++i)
                            nRand |= ((uint64_t)vchHash[(nHashIndex*8) + i]) << (8*i);

                        nHashIndex++;
                    } while (nRand > nRange);
                    CScriptNum result(nRand % nMax);
                    result += bnMin.getint();

                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(result.getvch());
                 }
                 break;

                case OP_CHECKSIGFROMSTACK:
                case OP_CHECKSIGFROMSTACKVERIFY:
                {
                    // (sig data pubkey  -- bool)
                    if (stack.size() < 3)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchSig    = stacktop(-3);
                    valtype& vchData   = stacktop(-2);
                    valtype& vchPubKey = stacktop(-1);
                    bool fSuccess;
                    // Different semantics for CHECKSIGFROMSTACK for taproot and pre-taproot
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0)
                    {
                        // Sigs from stack have no hash byte ever
                        if (!CheckSignatureEncoding(vchSig, (flags | SCRIPT_NO_SIGHASH_BYTE), serror) || !CheckPubKeyEncoding(vchPubKey, flags, sigversion, serror)) {
                            //serror is set
                            return false;
                        }

                        valtype vchHash(CSHA256::OUTPUT_SIZE);
                        CSHA256().Write(vchData.data(), vchData.size()).Finalize(vchHash.data());
                        uint256 hash(vchHash);

                        CPubKey pubkey(vchPubKey);
                        fSuccess = pubkey.Verify(hash, vchSig);
                        // CHECKSIGFROMSTACK in pre-tapscript cannot be failed.
                        if (!fSuccess)
                            return set_error(serror, SCRIPT_ERR_CHECKSIGVERIFY);
                    } else {
                        // New BIP 340 semantics for CHECKSIGFROMSTACK
                        if (!EvalTapScriptCheckSigFromStack(vchSig, vchPubKey, execdata, flags, vchData, sigversion, serror, fSuccess)) return false;
                    }
                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                    stack.push_back(fSuccess ? vchTrue : vchFalse);
                    if (opcode == OP_CHECKSIGFROMSTACKVERIFY)
                    {
                        if (fSuccess)
                            popstack(stack);
                        else
                            return set_error(serror, SCRIPT_ERR_CHECKSIGVERIFY);
                    }
                }
                break;

                case OP_SHA256INITIALIZE: // (in -- sha256_ctx)
                {
                    // OP_SHA256INITIALIZE is only available in Tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    CSHA256 ctx;
                    valtype& vch = stacktop(-1);
                    if (!ctx.SafeWrite(vch.data(), vch.size()))
                        return set_error(serror, SCRIPT_ERR_SHA2_CONTEXT_WRITE);

                    popstack(stack);
                    stack.push_back(ctx.Save());
                }
                break;

                case OP_SHA256UPDATE: // (sha256_ctx in -- sha256_ctx)
                {
                    // OP_SHA256UPDATE is only available in Tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    CSHA256 ctx;
                    valtype& vchCtx = stacktop(-2);
                    if (!ctx.Load(vchCtx))
                        return set_error(serror, SCRIPT_ERR_SHA2_CONTEXT_LOAD);

                    valtype& vch = stacktop(-1);
                    if (!ctx.SafeWrite(vch.data(), vch.size()))
                        return set_error(serror, SCRIPT_ERR_SHA2_CONTEXT_WRITE);

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(ctx.Save());
                }
                break;

                case OP_SHA256FINALIZE: // (sha256_ctx in -- hash)
                {
                    // OP_SHA256FINALIZE is only available in Tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchCtx = stacktop(-2);
                    CSHA256 ctx;
                    if (!ctx.Load(vchCtx))
                        return set_error(serror, SCRIPT_ERR_SHA2_CONTEXT_LOAD);

                    valtype& vch = stacktop(-1);
                    if (!ctx.SafeWrite(vch.data(), vch.size()))
                        return set_error(serror, SCRIPT_ERR_SHA2_CONTEXT_WRITE);

                    valtype vchHash(CHash256::OUTPUT_SIZE);
                    ctx.Finalize(vchHash.data());

                    popstack(stack);
                    popstack(stack);
                    stack.push_back(std::move(vchHash));
                }
                break;

                case OP_INSPECTINPUTOUTPOINT:
                case OP_INSPECTINPUTASSET:
                case OP_INSPECTINPUTVALUE:
                case OP_INSPECTINPUTSCRIPTPUBKEY:
                case OP_INSPECTINPUTSEQUENCE:
                case OP_INSPECTINPUTISSUANCE:
                {
                    // Input inspection opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int idx = CScriptNum(stacktop(-1), fRequireMinimal).getint();
                    popstack(stack);

                    auto inps = checker.GetTxvIn();
                    const PrecomputedTransactionData *cache = checker.GetPrecomputedTransactionData();
                    // Return error if the evaluation context is unavailable
                    // TODO: Handle according to MissingDataBehavior
                    if (!inps || !cache || !cache->m_bip341_taproot_ready)
                        return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                    const std::vector<CTxOut>& spent_outputs = cache->m_spent_outputs;
                    // This condition is ensured when m_spent_outputs_ready is set
                    // which is asserted when m_bip341_taproot_ready is set
                    assert(spent_outputs.size() == inps->size());
                    if (idx < 0 || static_cast<unsigned int>(idx) >= inps->size())
                        return set_error(serror, SCRIPT_ERR_INTROSPECT_INDEX_OUT_OF_BOUNDS);
                    const CTxIn& inp = inps->at(idx);
                    const CTxOut& spent_utxo = spent_outputs[idx];

                    switch (opcode)
                    {
                        case OP_INSPECTINPUTOUTPOINT:
                        {
                            // Push prev txid
                            stack.emplace_back(inp.prevout.hash.begin(), inp.prevout.hash.end());
                            push4_le(stack, inp.prevout.n);

                            // Push the outpoint flag
                            stack.emplace_back(1, GetOutpointFlag(inp));
                            break;
                        }
                        case OP_INSPECTINPUTASSET:
                        {
                            pushasset(stack, spent_utxo.nAsset);
                            break;
                        }
                        case OP_INSPECTINPUTVALUE:
                        {
                            pushvalue(stack, spent_utxo.nValue);
                            break;
                        }
                        case OP_INSPECTINPUTSCRIPTPUBKEY:
                        {
                            pushspk(stack, spent_utxo.scriptPubKey, cache->m_spent_output_spk_single_hashes[idx]);
                            break;
                        }
                        case OP_INSPECTINPUTSEQUENCE:
                        {
                            push4_le(stack, inp.nSequence);
                            break;
                        }
                        case OP_INSPECTINPUTISSUANCE:
                        {
                            if (!inp.assetIssuance.IsNull()) {
                                pushvalue(stack, inp.assetIssuance.nInflationKeys);
                                pushvalue(stack, inp.assetIssuance.nAmount);
                                // Next push Asset entropy
                                stack.emplace_back(inp.assetIssuance.assetEntropy.begin(), inp.assetIssuance.assetEntropy.end());
                                // Finally push blinding nonce
                                // By pushing the this order, we make sure that the stack top is empty
                                // iff there is no issuance.
                                stack.emplace_back(inp.assetIssuance.assetBlindingNonce.begin(), inp.assetIssuance.assetBlindingNonce.end());
                            } else { // No issuance
                                stack.push_back(vchFalse);
                            }
                            break;
                        }
                        default: assert(!"invalid opcode"); break;
                    }
                }
                break;

                case OP_PUSHCURRENTINPUTINDEX:
                {
                    // OP_PUSHCURRENTINPUTINDEX is available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    // Even tough this value should never 2^25(MAX_SIZE), this can set to any value in exotic custom contexts
                    // safe to check that this in 4 byte positive number before pushing it
                    // TODO: Handle according to MissingDataBehavior
                    if (checker.GetnIn() > MAX_SIZE)
                        return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                    stack.push_back(CScriptNum(static_cast<int64_t>(checker.GetnIn())).getvch());
                }
                break;

                case OP_INSPECTOUTPUTASSET:
                case OP_INSPECTOUTPUTVALUE:
                case OP_INSPECTOUTPUTNONCE:
                case OP_INSPECTOUTPUTSCRIPTPUBKEY:
                {
                    // Output instropsection codes only available post tapscript is available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int idx = CScriptNum(stacktop(-1), fRequireMinimal).getint();
                    popstack(stack);

                    auto outs = checker.GetTxvOut();
                    const PrecomputedTransactionData *cache = checker.GetPrecomputedTransactionData();
                    // Return error if the evaluation context is unavailable
                    // TODO: Handle according to MissingDataBehavior
                    if (!outs || !cache || !cache->m_bip341_taproot_ready)
                        return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                    assert(cache->m_output_spk_single_hashes.size() == outs->size());

                    if (idx < 0 || static_cast<unsigned int>(idx) >= outs->size())
                        return set_error(serror, SCRIPT_ERR_INTROSPECT_INDEX_OUT_OF_BOUNDS);
                    const CTxOut& out = outs->at(idx);

                    switch (opcode)
                    {
                        case OP_INSPECTOUTPUTASSET:
                        {
                            pushasset(stack, out.nAsset);
                            break;
                        }
                        case OP_INSPECTOUTPUTVALUE:
                        {
                            pushvalue(stack, out.nValue);
                            break;
                        }
                        case OP_INSPECTOUTPUTNONCE:
                        {
                            if (out.nNonce.IsNull()) {
                                stack.push_back(vchFalse);
                            } else {
                                stack.emplace_back(out.nNonce.vchCommitment);
                            }
                            break;
                        }
                        case OP_INSPECTOUTPUTSCRIPTPUBKEY:
                        {
                            pushspk(stack, out.scriptPubKey, cache->m_output_spk_single_hashes[idx]);
                            break;
                        }
                        default: assert(!"invalid opcode"); break;
                    }
                }
                break;

                case OP_INSPECTVERSION:
                case OP_INSPECTLOCKTIME:
                case OP_INSPECTNUMINPUTS:
                case OP_INSPECTNUMOUTPUTS:
                case OP_TXWEIGHT:
                {
                    // Transaction introspection is available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    switch (opcode)
                    {
                        case OP_INSPECTVERSION:
                        {
                            push4_le(stack, static_cast<uint32_t>(checker.GetTxVersion()));
                            break;
                        }
                        case OP_INSPECTLOCKTIME:
                        {
                            push4_le(stack, checker.GetLockTime());
                            break;
                        }
                        case OP_INSPECTNUMINPUTS:
                        {
                            auto inps = checker.GetTxvIn();
                            // TODO: Handle according to MissingDataBehavior
                            if (!inps)
                                return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                            auto num_ins = inps->size();
                            assert(num_ins <= MAX_SIZE);
                            stack.push_back(CScriptNum(static_cast<int64_t>(num_ins)).getvch());
                            break;
                        }
                        case OP_INSPECTNUMOUTPUTS:
                        {
                            auto outs = checker.GetTxvOut();
                            // TODO: Handle according to MissingDataBehavior
                            if (!outs)
                                return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                            auto num_outs = outs->size();
                            assert(num_outs <= MAX_SIZE);
                            stack.push_back(CScriptNum(static_cast<int64_t>(num_outs)).getvch());
                            break;
                        }
                        case OP_TXWEIGHT:
                        {
                            const PrecomputedTransactionData *cache = checker.GetPrecomputedTransactionData();
                            // Return error if the evaluation context is unavailable
                            // TODO: Handle according to MissingDataBehavior
                            if (!cache || !cache->m_bip341_taproot_ready)
                                return set_error(serror, SCRIPT_ERR_INTROSPECT_CONTEXT_UNAVAILABLE);
                            push8_le(stack, cache->m_tx_weight);
                            break;
                        }
                        default: assert(!"invalid opcode"); break;
                    }
                }
                break;

                case OP_ADD64:
                case OP_SUB64:
                case OP_MUL64:
                case OP_DIV64:
                case OP_LESSTHAN64:
                case OP_LESSTHANOREQUAL64:
                case OP_GREATERTHAN64:
                case OP_GREATERTHANOREQUAL64:
                {
                    // Opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 2)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vcha = stacktop(-2);
                    valtype& vchb = stacktop(-1);
                    if (vchb.size() != 8 || vcha.size() != 8)
                        return set_error(serror, SCRIPT_ERR_EXPECTED_8BYTES);

                    int64_t b = read_le8_signed(vchb.data());
                    int64_t a = read_le8_signed(vcha.data());

                    switch(opcode)
                    {
                        case OP_ADD64:
                            if ((a > 0 && b > std::numeric_limits<int64_t>::max() - a) ||
                                (a < 0 && b < std::numeric_limits<int64_t>::min() - a))
                                stack.push_back(vchFalse);
                            else {
                                popstack(stack);
                                popstack(stack);
                                push8_le(stack, a + b);
                                stack.push_back(vchTrue);
                            }
                        break;
                        case OP_SUB64:
                            if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
                                (b < 0 && a > std::numeric_limits<int64_t>::max() + b))
                                stack.push_back(vchFalse);
                            else {
                                popstack(stack);
                                popstack(stack);
                                push8_le(stack, a - b);
                                stack.push_back(vchTrue);
                            }
                        break;
                        case OP_MUL64:
                            if ((a > 0 && b > 0 && a > std::numeric_limits<int64_t>::max() / b) ||
                                (a > 0 && b < 0 && b < std::numeric_limits<int64_t>::min() / a) ||
                                (a < 0 && b > 0 && a < std::numeric_limits<int64_t>::min() / b) ||
                                (a < 0 && b < 0 && b < std::numeric_limits<int64_t>::max() / a))
                                stack.push_back(vchFalse);
                            else {
                                popstack(stack);
                                popstack(stack);
                                push8_le(stack, a * b);
                                stack.push_back(vchTrue);
                            }
                        break;
                        case OP_DIV64:
                        {
                            if (b == 0 || (b == -1 && a == std::numeric_limits<int64_t>::min())) { stack.push_back(vchFalse); break; }
                            int64_t r = a % b;
                            int64_t q = a / b;
                            if (r < 0 && b > 0)      { r += b; q-=1;} // ensures that 0<=r<|b|
                            else if (r < 0 && b < 0) { r -= b; q+=1;} // ensures that 0<=r<|b|
                            popstack(stack);
                            popstack(stack);
                            push8_le(stack, r);
                            push8_le(stack, q);
                            stack.push_back(vchTrue);
                        }
                        break;
                        break;
                        case OP_LESSTHAN64:            popstack(stack); popstack(stack); stack.push_back( (a <  b) ? vchTrue : vchFalse ); break;
                        case OP_LESSTHANOREQUAL64:     popstack(stack); popstack(stack); stack.push_back( (a <= b) ? vchTrue : vchFalse ); break;
                        case OP_GREATERTHAN64:         popstack(stack); popstack(stack); stack.push_back( (a >  b) ? vchTrue : vchFalse ); break;
                        case OP_GREATERTHANOREQUAL64:  popstack(stack); popstack(stack); stack.push_back( (a >= b) ? vchTrue : vchFalse ); break;
                        default:                       assert(!"invalid opcode"); break;
                    }
                }
                break;
                case OP_NEG64:
                {
                    // Opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vcha = stacktop(-1);
                    if (vcha.size() != 8)
                        return set_error(serror, SCRIPT_ERR_EXPECTED_8BYTES);

                    int64_t a = read_le8_signed(vcha.data());
                    if (a == std::numeric_limits<int64_t>::min()) { stack.push_back(vchFalse); break; }

                    popstack(stack);
                    push8_le(stack, -a);
                    stack.push_back(vchTrue);
                }
                break;

                case OP_SCRIPTNUMTOLE64:
                {
                    // Opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    int64_t num = CScriptNum(stacktop(-1), fRequireMinimal).getint();
                    popstack(stack);
                    push8_le(stack, num);
                }
                break;
                case OP_LE64TOSCRIPTNUM:
                {
                    // Opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchnum = stacktop(-1);
                    if (vchnum.size() != 8)
                        return set_error(serror, SCRIPT_ERR_EXPECTED_8BYTES);
                    valtype vchscript_num = CScriptNum(read_le8_signed(vchnum.data())).getvch();
                    if (vchscript_num.size() > CScriptNum::nDefaultMaxNumSize) {
                        return set_error(serror, SCRIPT_ERR_ARITHMETIC64);
                    } else {
                        popstack(stack);
                        stack.push_back(std::move(vchscript_num));
                    }
                }
                break;
                case OP_LE32TOLE64:
                {
                    // Opcodes only available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    if (stack.size() < 1)
                        return set_error(serror, SCRIPT_ERR_INVALID_STACK_OPERATION);

                    valtype& vchnum = stacktop(-1);
                    if (vchnum.size() != 4)
                        return set_error(serror, SCRIPT_ERR_ARITHMETIC64);
                    uint32_t num = ReadLE32(vchnum.data());
                    popstack(stack);
                    push8_le(stack, static_cast<int64_t>(num));
                }
                break;
                case OP_ECMULSCALARVERIFY:
                {
                    // OP_ECMULSCALARVERIFY is available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    valtype& vchRes = stacktop(-3);
                    valtype& vchGenerator = stacktop(-2);
                    valtype& vchScalar = stacktop(-1);

                    CPubKey pk(vchGenerator);
                    CPubKey res(vchRes);
                    if (!pk.IsCompressed() || !res.IsCompressed())
                        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);

                    if (!update_validation_weight(execdata, serror)) return false; // serror is set

                    if (vchScalar.size() != 32 || !res.TweakMulVerify(pk, uint256(vchScalar)))
                        return set_error(serror, SCRIPT_ERR_ECMULTVERIFYFAIL);

                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                }
                break;

                //crypto opcodes
                case OP_TWEAKVERIFY:
                {
                    // OP_TWEAKVERIFY is available post tapscript
                    if (sigversion == SigVersion::BASE || sigversion == SigVersion::WITNESS_V0) return set_error(serror, SCRIPT_ERR_BAD_OPCODE);

                    valtype& vchTweakedKey = stacktop(-3);
                    valtype& vchTweak = stacktop(-2);
                    valtype& vchInternalKey = stacktop(-1);

                    if (vchTweakedKey.size() != CPubKey::COMPRESSED_SIZE || (vchTweakedKey[0] != 0x02 && vchTweakedKey[0] != 0x03)
                        || vchInternalKey.size() != 32 || vchTweak.size() != 32)
                        return set_error(serror, SCRIPT_ERR_PUBKEYTYPE);

                    if (!update_validation_weight(execdata, serror)) return false; // serror is set

                    const XOnlyPubKey tweakedXOnlyKey{Span<const unsigned char>{vchTweakedKey.data() + 1, vchTweakedKey.data() + CPubKey::COMPRESSED_SIZE}};
                    const uint256 tweak(vchTweak);
                    const XOnlyPubKey internalKey{vchInternalKey};
                    if (!tweakedXOnlyKey.CheckPayToContract(internalKey, tweak, vchTweakedKey[0] & 1))
                        return set_error(serror, SCRIPT_ERR_ECMULTVERIFYFAIL);

                    popstack(stack);
                    popstack(stack);
                    popstack(stack);
                }
                break;

                default:
                    return set_error(serror, SCRIPT_ERR_BAD_OPCODE);
            }

            // Size limits
            if (stack.size() + altstack.size() > MAX_STACK_SIZE)
                return set_error(serror, SCRIPT_ERR_STACK_SIZE);
        }
    }
    catch (...)
    {
        return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);
    }

    if (!vfExec.empty())
        return set_error(serror, SCRIPT_ERR_UNBALANCED_CONDITIONAL);

    return set_success(serror);
}

bool EvalScript(std::vector<std::vector<unsigned char> >& stack, const CScript& script, unsigned int flags, const BaseSignatureChecker& checker, SigVersion sigversion, ScriptError* serror)
{
    ScriptExecutionData execdata;
    return EvalScript(stack, script, flags, checker, sigversion, execdata, serror);
}

namespace {

/**
 * Wrapper that serializes like CTransaction, but with the modifications
 *  required for the signature hash done in-place
 */
template <class T>
class CTransactionSignatureSerializer
{
private:
    const T& txTo;             //!< reference to the spending transaction (the one being serialized)
    const CScript& scriptCode; //!< output script being consumed
    const unsigned int nIn;    //!< input index of txTo being signed
    const bool fAnyoneCanPay;  //!< whether the hashtype has the SIGHASH_ANYONECANPAY flag set
    const bool fRangeproof;    //!< whether the hashtype has the SIGHASH_RANGEPROOF flag set
    const bool fHashSingle;    //!< whether the hashtype is SIGHASH_SINGLE
    const bool fHashNone;      //!< whether the hashtype is SIGHASH_NONE

public:
    CTransactionSignatureSerializer(const T& txToIn, const CScript& scriptCodeIn, unsigned int nInIn, int nHashTypeIn, unsigned int flags) :
        txTo(txToIn), scriptCode(scriptCodeIn), nIn(nInIn),
        fAnyoneCanPay(!!(nHashTypeIn & SIGHASH_ANYONECANPAY)),
        fRangeproof(!!(flags & SCRIPT_SIGHASH_RANGEPROOF) && !!(nHashTypeIn & SIGHASH_RANGEPROOF)),
        fHashSingle((nHashTypeIn & 0x1f) == SIGHASH_SINGLE),
        fHashNone((nHashTypeIn & 0x1f) == SIGHASH_NONE) {}

    /** Serialize the passed scriptCode, skipping OP_CODESEPARATORs */
    template<typename S>
    void SerializeScriptCode(S &s) const {
        CScript::const_iterator it = scriptCode.begin();
        CScript::const_iterator itBegin = it;
        opcodetype opcode;
        unsigned int nCodeSeparators = 0;
        while (scriptCode.GetOp(it, opcode)) {
            if (opcode == OP_CODESEPARATOR)
                nCodeSeparators++;
        }
        ::WriteCompactSize(s, scriptCode.size() - nCodeSeparators);
        it = itBegin;
        while (scriptCode.GetOp(it, opcode)) {
            if (opcode == OP_CODESEPARATOR) {
                s.write(AsBytes(Span{&itBegin[0], size_t(it - itBegin - 1)}));
                itBegin = it;
            }
        }
        if (itBegin != scriptCode.end())
            s.write(AsBytes(Span{&itBegin[0], size_t(it - itBegin)}));
    }

    /** Serialize an input of txTo */
    template<typename S>
    void SerializeInput(S &s, unsigned int nInput) const {
        // In case of SIGHASH_ANYONECANPAY, only the input being signed is serialized
        if (fAnyoneCanPay)
            nInput = nIn;
        // Serialize the prevout
        ::Serialize(s, txTo.vin[nInput].prevout);
        // Serialize the script
        if (nInput != nIn)
            // Blank out other inputs' signatures
            ::Serialize(s, CScript());
        else
            SerializeScriptCode(s);
        // Serialize the nSequence
        if (nInput != nIn && (fHashSingle || fHashNone))
            // let the others update at will
            ::Serialize(s, (int)0);
        else
            ::Serialize(s, txTo.vin[nInput].nSequence);
        // Serialize the asset issuance object
        if (!txTo.vin[nInput].assetIssuance.IsNull()) {
            assert(g_con_elementsmode);
            ::Serialize(s, txTo.vin[nInput].assetIssuance);
        }
    }

    /** Serialize an output of txTo */
    template<typename S>
    void SerializeOutput(S &s, unsigned int nOutput) const {
        if (fHashSingle && nOutput != nIn) {
            // Do not lock-in the txout payee at other indices as txin
            ::Serialize(s, CTxOut());
        } else {
            ::Serialize(s, txTo.vout[nOutput]);

            // Serialize rangeproof
            if (fRangeproof) {
                if (nOutput < txTo.witness.vtxoutwit.size()) {
                    ::Serialize(s, txTo.witness.vtxoutwit[nOutput].vchRangeproof);
                    ::Serialize(s, txTo.witness.vtxoutwit[nOutput].vchSurjectionproof);
                } else {
                    ::Serialize(s, (unsigned char) 0);
                    ::Serialize(s, (unsigned char) 0);
                }
            }
        }
    }

    /** Serialize txTo */
    template<typename S>
    void Serialize(S &s) const {
        // Serialize nVersion
        ::Serialize(s, txTo.nVersion);
        // Serialize vin
        unsigned int nInputs = fAnyoneCanPay ? 1 : txTo.vin.size();
        ::WriteCompactSize(s, nInputs);
        for (unsigned int nInput = 0; nInput < nInputs; nInput++)
             SerializeInput(s, nInput);
        // Serialize vout
        unsigned int nOutputs = fHashNone ? 0 : (fHashSingle ? nIn+1 : txTo.vout.size());
        ::WriteCompactSize(s, nOutputs);
        for (unsigned int nOutput = 0; nOutput < nOutputs; nOutput++)
             SerializeOutput(s, nOutput);
        // Serialize nLockTime
        ::Serialize(s, txTo.nLockTime);
    }
};

/** Compute the (single) SHA256 of the concatenation of all outpoint flags of a tx. */
template <class T>
uint256 GetOutpointFlagsSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txin : txTo.vin) {
        ss << GetOutpointFlag(txin);
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all prevouts of a tx. */
template <class T>
uint256 GetPrevoutsSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txin : txTo.vin) {
        ss << txin.prevout;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all nSequences of a tx. */
template <class T>
uint256 GetSequencesSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txin : txTo.vin) {
        ss << txin.nSequence;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all issuances of a tx. */
// Used for segwitv0/taproot sighash calculation
template <class T>
uint256 GetIssuanceSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txin : txTo.vin) {
        if (txin.assetIssuance.IsNull())
            ss << (unsigned char)0;
        else
            ss << txin.assetIssuance;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all output witnesses
 * (rangeproof and surjection proof) in `CTxWitness`*/
// Used in taphash calculation
template <class T>
uint256 GetOutputWitnessesSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& outwit : txTo.witness.vtxoutwit) {
        ss << outwit;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all input issuance witnesses
 * (vchIssuanceAmountRangeproof and vchInflationKeysRangeproof proof) in `CTxInWitness`*/
// Used in taphash calculation
template <class T>
uint256 GetIssuanceRangeproofsSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& inwit : txTo.witness.vtxinwit) {
        ss << inwit.vchIssuanceAmountRangeproof;
        ss << inwit.vchInflationKeysRangeproof;
    }
    return ss.GetSHA256();
}

// Compute a (single) SHA256 of the concatenation of all outputs
template <class T>
uint256 GetOutputsSHA256(const T& txTo)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txout : txTo.vout) {
        ss << txout;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all asset and amounts commitments spent by a tx. */
// Elements TapHash only
uint256 GetSpentAssetsAmountsSHA256(const std::vector<CTxOut>& outputs_spent)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txout : outputs_spent) {
        ss << txout.nAsset;
        ss << txout.nValue;
    }
    return ss.GetSHA256();
}

/** Compute the (single) SHA256 of the concatenation of all scriptPubKeys spent by a tx. */
uint256 GetSpentScriptsSHA256(const std::vector<CTxOut>& outputs_spent)
{
    CHashWriter ss(SER_GETHASH, 0);
    for (const auto& txout : outputs_spent) {
        ss << txout.scriptPubKey;
    }
    return ss.GetSHA256();
}

/** Compute the vector where each element is SHA256 of scriptPubKeys spent by a tx. */
std::vector<uint256> GetSpentScriptPubKeysSHA256(const std::vector<CTxOut>& outputs_spent)
{
    std::vector<uint256> spent_spk_single_hashes;
    spent_spk_single_hashes.reserve(outputs_spent.size());
    for (const auto& txout : outputs_spent) {
        // Normal serialization using the << operator would also serialize the length, therefore we directly write using CSHA256
        uint256 spent_spk_single_hash;
        CSHA256().Write(txout.scriptPubKey.data(), txout.scriptPubKey.size()).Finalize(spent_spk_single_hash.data());
        spent_spk_single_hashes.push_back(std::move(spent_spk_single_hash));
    }
    return spent_spk_single_hashes;
}

/** Compute the vector where each element is SHA256 of output scriptPubKey of a tx. */
template <class T>
std::vector<uint256> GetOutputScriptPubKeysSHA256(const T& txTo)
{
    std::vector<uint256> out_spk_single_hashes;
    out_spk_single_hashes.reserve(txTo.vout.size());
    for (const auto& txout : txTo.vout) {
        // Normal serialization using the << operator would also serialize the length, therefore we directly write using CSHA256
        uint256 out_spk_single_hash;
        CSHA256().Write(txout.scriptPubKey.data(), txout.scriptPubKey.size()).Finalize(out_spk_single_hash.data());
        out_spk_single_hashes.push_back(std::move(out_spk_single_hash));
    }
    return out_spk_single_hashes;
}

template <class T>
uint256 GetRangeproofsHash(const T& txTo) {
    CHashWriter ss(SER_GETHASH, 0);
    for (size_t i = 0; i < txTo.vout.size(); i++) {
        if (i < txTo.witness.vtxoutwit.size()) {
            ss << txTo.witness.vtxoutwit[i].vchRangeproof;
            ss << txTo.witness.vtxoutwit[i].vchSurjectionproof;
        } else {
            ss << (unsigned char) 0;
            ss << (unsigned char) 0;
        }
    }
    return ss.GetHash();
}

} // namespace

template <class T>
void PrecomputedTransactionData::Init(const T& txTo, std::vector<CTxOut>&& spent_outputs, bool force)
{
    assert(!m_spent_outputs_ready);

    m_spent_outputs = std::move(spent_outputs);
    if (!m_spent_outputs.empty()) {
        assert(m_spent_outputs.size() == txTo.vin.size());
        m_spent_outputs_ready = true;
    }

    // Determine which precomputation-impacting features this transaction uses.
    bool uses_bip143_segwit = force;
    bool uses_bip341_taproot = force;
    for (size_t inpos = 0; inpos < txTo.vin.size() && !(uses_bip143_segwit && uses_bip341_taproot); ++inpos) {
        if (inpos < txTo.witness.vtxinwit.size() && !txTo.witness.vtxinwit[inpos].scriptWitness.IsNull()) {
            if (m_spent_outputs_ready && m_spent_outputs[inpos].scriptPubKey.size() == 2 + WITNESS_V1_TAPROOT_SIZE &&
                m_spent_outputs[inpos].scriptPubKey[0] == OP_1) {
                // Treat every witness-bearing spend with 34-byte scriptPubKey that starts with OP_1 as a Taproot
                // spend. This only works if spent_outputs was provided as well, but if it wasn't, actual validation
                // will fail anyway. Note that this branch may trigger for scriptPubKeys that aren't actually segwit
                // but in that case validation will fail as SCRIPT_ERR_WITNESS_UNEXPECTED anyway.
                uses_bip341_taproot = true;
            } else {
                // Treat every spend that's not known to native witness v1 as a Witness v0 spend. This branch may
                // also be taken for unknown witness versions, but it is harmless, and being precise would require
                // P2SH evaluation to find the redeemScript.
                uses_bip143_segwit = true;
            }
        }
        if (uses_bip341_taproot && uses_bip143_segwit) break; // No need to scan further if we already need all.
    }

    if (uses_bip143_segwit || uses_bip341_taproot) {
        // Computations shared between both sighash schemes.
        m_prevouts_single_hash = GetPrevoutsSHA256(txTo);
        m_sequences_single_hash = GetSequencesSHA256(txTo);
        m_outputs_single_hash = GetOutputsSHA256(txTo);
        m_issuances_single_hash = GetIssuanceSHA256(txTo);
    }
    if (uses_bip143_segwit) {
        hashPrevouts = SHA256Uint256(m_prevouts_single_hash);
        hashSequence = SHA256Uint256(m_sequences_single_hash);
        hashIssuance = SHA256Uint256(m_issuances_single_hash);
        hashOutputs = SHA256Uint256(m_outputs_single_hash);
        hashRangeproofs = GetRangeproofsHash(txTo);
        m_bip143_segwit_ready = true;
    }
    if (uses_bip341_taproot && m_spent_outputs_ready) {
        // line copied from GetTransactionWeight() in src/consensus/validation.h
        // (we cannot directly use that function for type reasons)
        m_tx_weight = ::GetSerializeSize(txTo, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * (WITNESS_SCALE_FACTOR - 1) + ::GetSerializeSize(txTo, PROTOCOL_VERSION);
        m_outpoints_flag_single_hash = GetOutpointFlagsSHA256(txTo);
        m_spent_asset_amounts_single_hash = GetSpentAssetsAmountsSHA256(m_spent_outputs);
        m_issuance_rangeproofs_single_hash = GetIssuanceRangeproofsSHA256(txTo);
        m_output_witnesses_single_hash = GetOutputWitnessesSHA256(txTo);
        m_spent_scripts_single_hash = GetSpentScriptsSHA256(m_spent_outputs);
        m_spent_output_spk_single_hashes = GetSpentScriptPubKeysSHA256(m_spent_outputs);
        m_output_spk_single_hashes = GetOutputScriptPubKeysSHA256(txTo);

        std::vector<rawElementsBuffer> simplicityRawAnnex(txTo.witness.vtxinwit.size());
        std::vector<rawElementsInput> simplicityRawInput(txTo.vin.size());
        for (size_t i = 0; i < txTo.vin.size(); ++i) {
            simplicityRawInput[i].prevTxid = txTo.vin[i].prevout.hash.begin();
            simplicityRawInput[i].prevIx = txTo.vin[i].prevout.n;
            simplicityRawInput[i].sequence = txTo.vin[i].nSequence;
            simplicityRawInput[i].txo.asset = m_spent_outputs[i].nAsset.vchCommitment.empty() ? NULL : m_spent_outputs[i].nAsset.vchCommitment.data();
            simplicityRawInput[i].txo.value = m_spent_outputs[i].nValue.vchCommitment.empty() ? NULL : m_spent_outputs[i].nValue.vchCommitment.data();
            simplicityRawInput[i].txo.scriptPubKey.buf = m_spent_outputs[i].scriptPubKey.data();
            simplicityRawInput[i].txo.scriptPubKey.len = m_spent_outputs[i].scriptPubKey.size();
            simplicityRawInput[i].issuance.blindingNonce = txTo.vin[i].assetIssuance.assetBlindingNonce.begin();
            simplicityRawInput[i].issuance.assetEntropy = txTo.vin[i].assetIssuance.assetEntropy.begin();
            simplicityRawInput[i].issuance.amount = txTo.vin[i].assetIssuance.nAmount.vchCommitment.empty() ? NULL : txTo.vin[i].assetIssuance.nAmount.vchCommitment.data();
            simplicityRawInput[i].issuance.inflationKeys = txTo.vin[i].assetIssuance.nInflationKeys.vchCommitment.empty() ? NULL : txTo.vin[i].assetIssuance.nInflationKeys.vchCommitment.data();
            simplicityRawInput[i].annex = NULL;
            if (i < txTo.witness.vtxinwit.size()) {
                Span<const valtype> stack{txTo.witness.vtxinwit[i].scriptWitness.stack};
                if (stack.size() >= 2 && !stack.back().empty() && stack.back()[0] == ANNEX_TAG) {
                    simplicityRawAnnex[i].buf = stack.back().data()+1;
                    simplicityRawAnnex[i].len = stack.back().size()-1;
                    simplicityRawInput[i].annex = &simplicityRawAnnex[i];
                }
                simplicityRawInput[i].issuance.amountRangePrf.buf = txTo.witness.vtxinwit[i].vchIssuanceAmountRangeproof.data();
                simplicityRawInput[i].issuance.amountRangePrf.len = txTo.witness.vtxinwit[i].vchIssuanceAmountRangeproof.size();
                simplicityRawInput[i].issuance.inflationKeysRangePrf.buf = txTo.witness.vtxinwit[i].vchInflationKeysRangeproof.data();
                simplicityRawInput[i].issuance.inflationKeysRangePrf.len = txTo.witness.vtxinwit[i].vchInflationKeysRangeproof.size();
                assert(!txTo.vin[i].m_is_pegin || ( txTo.witness.vtxinwit[i].m_pegin_witness.stack.size() >= 4 && txTo.witness.vtxinwit[i].m_pegin_witness.stack[2].size() == 32));
                simplicityRawInput[i].pegin = txTo.vin[i].m_is_pegin ? txTo.witness.vtxinwit[i].m_pegin_witness.stack[2].data() : 0;
            } else {
                simplicityRawInput[i].issuance.amountRangePrf.buf = NULL;
                simplicityRawInput[i].issuance.amountRangePrf.len = 0;
                simplicityRawInput[i].issuance.inflationKeysRangePrf.buf = NULL;
                simplicityRawInput[i].issuance.inflationKeysRangePrf.len = 0;
                assert(!txTo.vin[i].m_is_pegin);
                simplicityRawInput[i].pegin = 0;
            }
        }

        std::vector<rawElementsOutput> simplicityRawOutput(txTo.vout.size());
        for (size_t i = 0; i < txTo.vout.size(); ++i) {
            simplicityRawOutput[i].asset = txTo.vout[i].nAsset.vchCommitment.empty() ? NULL : txTo.vout[i].nAsset.vchCommitment.data();
            simplicityRawOutput[i].value = txTo.vout[i].nValue.vchCommitment.empty() ? NULL : txTo.vout[i].nValue.vchCommitment.data();
            simplicityRawOutput[i].nonce = txTo.vout[i].nNonce.vchCommitment.empty() ? NULL : txTo.vout[i].nNonce.vchCommitment.data();
            simplicityRawOutput[i].scriptPubKey.buf = txTo.vout[i].scriptPubKey.data();
            simplicityRawOutput[i].scriptPubKey.len = txTo.vout[i].scriptPubKey.size();
            if (i < txTo.witness.vtxoutwit.size()) {
                simplicityRawOutput[i].surjectionProof.buf = txTo.witness.vtxoutwit[i].vchSurjectionproof.data();
                simplicityRawOutput[i].surjectionProof.len = txTo.witness.vtxoutwit[i].vchSurjectionproof.size();
                simplicityRawOutput[i].rangeProof.buf = txTo.witness.vtxoutwit[i].vchRangeproof.data();
                simplicityRawOutput[i].rangeProof.len = txTo.witness.vtxoutwit[i].vchRangeproof.size();
            } else {
                simplicityRawOutput[i].surjectionProof.buf = NULL;
                simplicityRawOutput[i].surjectionProof.len = 0;
                simplicityRawOutput[i].rangeProof.buf = NULL;
                simplicityRawOutput[i].rangeProof.len = 0;
            }
        }

        rawElementsTransaction simplicityRawTx;
        uint256 rawHash = txTo.GetHash();
        simplicityRawTx.txid = rawHash.begin();
        simplicityRawTx.input = simplicityRawInput.data();
        simplicityRawTx.numInputs = simplicityRawInput.size();
        simplicityRawTx.output = simplicityRawOutput.data();
        simplicityRawTx.numOutputs = simplicityRawOutput.size();
        simplicityRawTx.version = txTo.nVersion;
        simplicityRawTx.lockTime = txTo.nLockTime;

        m_simplicity_tx_data = SimplicityTransactionUniquePtr(simplicity_elements_mallocTransaction(&simplicityRawTx));

        m_bip341_taproot_ready = true;
    }
}

template <class T>
PrecomputedTransactionData::PrecomputedTransactionData(const T& txTo)
     : PrecomputedTransactionData(uint256{})
{
    Init(txTo, {});
}

// explicit instantiation
template void PrecomputedTransactionData::Init(const CTransaction& txTo, std::vector<CTxOut>&& spent_outputs, bool force);
template void PrecomputedTransactionData::Init(const CMutableTransaction& txTo, std::vector<CTxOut>&& spent_outputs, bool force);
template PrecomputedTransactionData::PrecomputedTransactionData(const CTransaction& txTo);
template PrecomputedTransactionData::PrecomputedTransactionData(const CMutableTransaction& txTo);

PrecomputedTransactionData::PrecomputedTransactionData(const uint256& hash_genesis_block)
        : m_hash_genesis_block(hash_genesis_block)
        , m_tapsighash_hasher(CHashWriter(HASHER_TAPSIGHASH_ELEMENTS) << hash_genesis_block << hash_genesis_block) {}

static bool HandleMissingData(MissingDataBehavior mdb)
{
    switch (mdb) {
    case MissingDataBehavior::ASSERT_FAIL:
        assert(!"Missing data");
        break;
    case MissingDataBehavior::FAIL:
        return false;
    }
    assert(!"Unknown MissingDataBehavior value");
}

template<typename T>
bool SignatureHashSchnorr(uint256& hash_out, ScriptExecutionData& execdata, const T& tx_to, uint32_t in_pos, uint8_t hash_type, SigVersion sigversion, const PrecomputedTransactionData& cache, MissingDataBehavior mdb)
{
    uint8_t ext_flag, key_version;
    switch (sigversion) {
    case SigVersion::TAPROOT:
        ext_flag = 0;
        // key_version is not used and left uninitialized.
        break;
    case SigVersion::TAPSCRIPT:
        ext_flag = 1;
        // key_version must be 0 for now, representing the current version of
        // 32-byte public keys in the tapscript signature opcode execution.
        // An upgradable public key version (with a size not 32-byte) may
        // request a different key_version with a new sigversion.
        key_version = 0;
        break;
    default:
        assert(false);
    }
    assert(in_pos < tx_to.vin.size());
    if (!(cache.m_bip341_taproot_ready && cache.m_spent_outputs_ready)) {
        return HandleMissingData(mdb);
    }

    CHashWriter ss = cache.m_tapsighash_hasher;

    // no epoch in elements taphash
    // static constexpr uint8_t EPOCH = 0;
    // ss << EPOCH;

    // Hash type
    const uint8_t output_type = (hash_type == SIGHASH_DEFAULT) ? SIGHASH_ALL : (hash_type & SIGHASH_OUTPUT_MASK); // Default (no sighash byte) is equivalent to SIGHASH_ALL
    const uint8_t input_type = hash_type & SIGHASH_INPUT_MASK;
    if (!(hash_type <= 0x03 || (hash_type >= 0x81 && hash_type <= 0x83))) return false;
    ss << hash_type;

    // Transaction level data
    ss << tx_to.nVersion;
    ss << tx_to.nLockTime;
    if (input_type != SIGHASH_ANYONECANPAY) {
        ss << cache.m_outpoints_flag_single_hash;
        ss << cache.m_prevouts_single_hash;
        ss << cache.m_spent_asset_amounts_single_hash;
        // Why is nNonce not included in sighash?(both in ACP and non ACP case)
        //
        // Nonces are not serialized into utxo database. As a consequence, after restarting the node,
        // all nonces in the utxoset are cleared which results in a inconsistent view for nonces for
        // nodes that did not restart. See https://github.com/ElementsProject/elements/issues/1004 for details
        ss << cache.m_spent_scripts_single_hash;
        ss << cache.m_sequences_single_hash;
        ss << cache.m_issuances_single_hash;
        ss << cache.m_issuance_rangeproofs_single_hash;
    }
    if (output_type == SIGHASH_ALL) {
        ss << cache.m_outputs_single_hash;
        ss << cache.m_output_witnesses_single_hash;
    }
    // Data about the input/prevout being spent
    assert(execdata.m_annex_init);
    const bool have_annex = execdata.m_annex_present;
    const uint8_t spend_type = (ext_flag << 1) + (have_annex ? 1 : 0); // The low bit indicates whether an annex is present.
    ss << spend_type;
    if (input_type == SIGHASH_ANYONECANPAY) {
        ss << GetOutpointFlag(tx_to.vin[in_pos]);
        ss << tx_to.vin[in_pos].prevout;
        ss << cache.m_spent_outputs[in_pos].nAsset;
        ss << cache.m_spent_outputs[in_pos].nValue;
        ss << cache.m_spent_outputs[in_pos].scriptPubKey;
        ss << tx_to.vin[in_pos].nSequence;
        if (tx_to.vin[in_pos].assetIssuance.IsNull()) {
            ss << (unsigned char)0;
        } else {
            ss << tx_to.vin[in_pos].assetIssuance;

            CHashWriter sha_single_input_issuance_witness(SER_GETHASH, 0);
            sha_single_input_issuance_witness << tx_to.witness.vtxinwit[in_pos].vchIssuanceAmountRangeproof;
            sha_single_input_issuance_witness << tx_to.witness.vtxinwit[in_pos].vchInflationKeysRangeproof;
            ss << sha_single_input_issuance_witness.GetSHA256();
        }
    } else {
        ss << in_pos;
    }
    if (have_annex) {
        ss << execdata.m_annex_hash;
    }
    // Data about the output (if only one).
    if (output_type == SIGHASH_SINGLE) {
        if (in_pos >= tx_to.vout.size()) return false;
        if (!execdata.m_output_hash) {
            CHashWriter sha_single_output(SER_GETHASH, 0);
            sha_single_output << tx_to.vout[in_pos];
            execdata.m_output_hash = sha_single_output.GetSHA256();
        }
        ss << execdata.m_output_hash.value();

        // ELEMENTS
        if (!execdata.m_output_witness_hash) {
            CHashWriter sha_single_output_witness(SER_GETHASH, 0);
            sha_single_output_witness << tx_to.witness.vtxoutwit[in_pos];
            execdata.m_output_witness_hash = sha_single_output_witness.GetSHA256();
        }
        ss << execdata.m_output_witness_hash.value();
    }

    // Additional data for BIP 342 signatures
    if (sigversion == SigVersion::TAPSCRIPT) {
        assert(execdata.m_tapleaf_hash_init);
        ss << execdata.m_tapleaf_hash;
        ss << key_version;
        assert(execdata.m_codeseparator_pos_init);
        ss << execdata.m_codeseparator_pos;
    }

    hash_out = ss.GetSHA256();
    return true;
}

template <class T>
uint256 SignatureHash(const CScript& scriptCode, const T& txTo, unsigned int nIn, int nHashType, const CConfidentialValue& amount, SigVersion sigversion, unsigned int flags, const PrecomputedTransactionData* cache)
{
    assert(nIn < txTo.vin.size());

    if (sigversion == SigVersion::WITNESS_V0) {
        uint256 hashPrevouts;
        uint256 hashSequence;
        uint256 hashIssuance;
        uint256 hashOutputs;
        uint256 hashRangeproofs;
        const bool cacheready = cache && cache->m_bip143_segwit_ready;
        bool fRangeproof = !!(flags & SCRIPT_SIGHASH_RANGEPROOF) && !!(nHashType & SIGHASH_RANGEPROOF);

        if (!(nHashType & SIGHASH_ANYONECANPAY)) {
            hashPrevouts = cacheready ? cache->hashPrevouts : SHA256Uint256(GetPrevoutsSHA256(txTo));
        }

        if (!(nHashType & SIGHASH_ANYONECANPAY) && (nHashType & 0x1f) != SIGHASH_SINGLE && (nHashType & 0x1f) != SIGHASH_NONE) {
            hashSequence = cacheready ? cache->hashSequence : SHA256Uint256(GetSequencesSHA256(txTo));
        }

        if (!(nHashType & SIGHASH_ANYONECANPAY)) {
            hashIssuance = cacheready ? cache->hashIssuance : SHA256Uint256(GetIssuanceSHA256(txTo));
        }

        if ((nHashType & 0x1f) != SIGHASH_SINGLE && (nHashType & 0x1f) != SIGHASH_NONE) {
            hashOutputs = cacheready ? cache->hashOutputs : SHA256Uint256(GetOutputsSHA256(txTo));

            if (fRangeproof) {
                hashRangeproofs = cacheready ? cache->hashRangeproofs : GetRangeproofsHash(txTo);
            }
        } else if ((nHashType & 0x1f) == SIGHASH_SINGLE && nIn < txTo.vout.size()) {
            CHashWriter ss(SER_GETHASH, 0);
            ss << txTo.vout[nIn];
            hashOutputs = ss.GetHash();

            if (fRangeproof) {
                CHashWriter ss(SER_GETHASH, 0);
                if (nIn < txTo.witness.vtxoutwit.size()) {
                    ss << txTo.witness.vtxoutwit[nIn].vchRangeproof;
                    ss << txTo.witness.vtxoutwit[nIn].vchSurjectionproof;
                } else {
                    ss << (unsigned char) 0;
                    ss << (unsigned char) 0;
                }
                hashRangeproofs = ss.GetHash();
            }
        }

        CHashWriter ss(SER_GETHASH, 0);
        // Version
        ss << txTo.nVersion;
        // Input prevouts/nSequence (none/all, depending on flags)
        ss << hashPrevouts;
        ss << hashSequence;
        if (g_con_elementsmode) {
            ss << hashIssuance;
        }
        // The input being signed (replacing the scriptSig with scriptCode + amount)
        // The prevout may already be contained in hashPrevout, and the nSequence
        // may already be contain in hashSequence.
        ss << txTo.vin[nIn].prevout;
        ss << scriptCode;
        if (g_con_elementsmode) {
            ss << amount;
        } else {
            ss << amount.GetAmount();
        }
        ss << txTo.vin[nIn].nSequence;
        if (!txTo.vin[nIn].assetIssuance.IsNull()) {
            assert(g_con_elementsmode);
            ss << txTo.vin[nIn].assetIssuance;
        }
        // Outputs (none/one/all, depending on flags)
        ss << hashOutputs;
        if (fRangeproof) {
            // This addition must be conditional because it was added after
            // the segwit sighash was specified.
            ss << hashRangeproofs;
        }
        // Locktime
        ss << txTo.nLockTime;
        // Sighash type
        ss << nHashType;

        return ss.GetHash();
    }

    // Check for invalid use of SIGHASH_SINGLE
    if ((nHashType & 0x1f) == SIGHASH_SINGLE) {
        if (nIn >= txTo.vout.size()) {
            //  nOut out of range
            return uint256::ONE;
        }
    }

    // Wrapper to serialize only the necessary parts of the transaction being signed
    CTransactionSignatureSerializer<T> txTmp(txTo, scriptCode, nIn, nHashType, flags);

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

template <class T>
bool GenericTransactionSignatureChecker<T>::VerifyECDSASignature(const std::vector<unsigned char>& vchSig, const CPubKey& pubkey, const uint256& sighash) const
{
    return pubkey.Verify(sighash, vchSig);
}

template <class T>
bool GenericTransactionSignatureChecker<T>::VerifySchnorrSignature(Span<const unsigned char> sig, const XOnlyPubKey& pubkey, const uint256& sighash) const
{
    return pubkey.VerifySchnorr(sighash, sig);
}

template <class T>
bool GenericTransactionSignatureChecker<T>::CheckECDSASignature(const std::vector<unsigned char>& vchSigIn, const std::vector<unsigned char>& vchPubKey, const CScript& scriptCode, SigVersion sigversion, unsigned int flags) const
{
    CPubKey pubkey(vchPubKey);
    if (!pubkey.IsValid())
        return false;

    // Hash type is one byte tacked on to the end of the signature
    std::vector<unsigned char> vchSig(vchSigIn);
    if (vchSig.empty())
        return false;
    int nHashType = vchSig.back();
    vchSig.pop_back();

    // Witness sighashes need the amount.
    if (sigversion == SigVersion::WITNESS_V0 && amount.IsNull()) return HandleMissingData(m_mdb);

    uint256 sighash = SignatureHash(scriptCode, *txTo, nIn, nHashType, amount, sigversion, flags, this->txdata);

    if (!VerifyECDSASignature(vchSig, pubkey, sighash))
        return false;

    return true;
}

template <class T>
bool GenericTransactionSignatureChecker<T>::CheckSchnorrSignature(Span<const unsigned char> sig, Span<const unsigned char> pubkey_in, SigVersion sigversion, ScriptExecutionData& execdata, ScriptError* serror) const
{
    assert(sigversion == SigVersion::TAPROOT || sigversion == SigVersion::TAPSCRIPT);
    // Schnorr signatures have 32-byte public keys. The caller is responsible for enforcing this.
    assert(pubkey_in.size() == 32);
    // Note that in Tapscript evaluation, empty signatures are treated specially (invalid signature that does not
    // abort script execution). This is implemented in EvalChecksigTapscript, which won't invoke
    // CheckSchnorrSignature in that case. In other contexts, they are invalid like every other signature with
    // size different from 64 or 65.
    if (sig.size() != 64 && sig.size() != 65) return set_error(serror, SCRIPT_ERR_SCHNORR_SIG_SIZE);

    XOnlyPubKey pubkey{pubkey_in};

    uint8_t hashtype = SIGHASH_DEFAULT;
    if (sig.size() == 65) {
        hashtype = SpanPopBack(sig);
        if (hashtype == SIGHASH_DEFAULT) return set_error(serror, SCRIPT_ERR_SCHNORR_SIG_HASHTYPE);
    }
    uint256 sighash;
    if (!this->txdata) return HandleMissingData(m_mdb);
    if (!SignatureHashSchnorr(sighash, execdata, *txTo, nIn, hashtype, sigversion, *this->txdata, m_mdb)) {
        return set_error(serror, SCRIPT_ERR_SCHNORR_SIG_HASHTYPE);
    }
    if (!VerifySchnorrSignature(sig, pubkey, sighash)) return set_error(serror, SCRIPT_ERR_SCHNORR_SIG);
    return true;
}

template <class T>
bool GenericTransactionSignatureChecker<T>::CheckLockTime(const CScriptNum& nLockTime) const
{
    // There are two kinds of nLockTime: lock-by-blockheight
    // and lock-by-blocktime, distinguished by whether
    // nLockTime < LOCKTIME_THRESHOLD.
    //
    // We want to compare apples to apples, so fail the script
    // unless the type of nLockTime being tested is the same as
    // the nLockTime in the transaction.
    if (!(
        (txTo->nLockTime <  LOCKTIME_THRESHOLD && nLockTime <  LOCKTIME_THRESHOLD) ||
        (txTo->nLockTime >= LOCKTIME_THRESHOLD && nLockTime >= LOCKTIME_THRESHOLD)
    ))
        return false;

    // Now that we know we're comparing apples-to-apples, the
    // comparison is a simple numeric one.
    if (nLockTime > (int64_t)txTo->nLockTime)
        return false;

    // Finally the nLockTime feature can be disabled in IsFinalTx()
    // and thus CHECKLOCKTIMEVERIFY bypassed if every txin has
    // been finalized by setting nSequence to maxint. The
    // transaction would be allowed into the blockchain, making
    // the opcode ineffective.
    //
    // Testing if this vin is not final is sufficient to
    // prevent this condition. Alternatively we could test all
    // inputs, but testing just this input minimizes the data
    // required to prove correct CHECKLOCKTIMEVERIFY execution.
    if (CTxIn::SEQUENCE_FINAL == txTo->vin[nIn].nSequence)
        return false;

    return true;
}

template <class T>
bool GenericTransactionSignatureChecker<T>::CheckSequence(const CScriptNum& nSequence) const
{
    // Relative lock times are supported by comparing the passed
    // in operand to the sequence number of the input.
    const int64_t txToSequence = (int64_t)txTo->vin[nIn].nSequence;

    // Fail if the transaction's version number is not set high
    // enough to trigger BIP 68 rules.
    if (static_cast<uint32_t>(txTo->nVersion) < 2)
        return false;

    // Sequence numbers with their most significant bit set are not
    // consensus constrained. Testing that the transaction's sequence
    // number do not have this bit set prevents using this property
    // to get around a CHECKSEQUENCEVERIFY check.
    if (txToSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG)
        return false;

    // Mask off any bits that do not have consensus-enforced meaning
    // before doing the integer comparisons
    const uint32_t nLockTimeMask = CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG | CTxIn::SEQUENCE_LOCKTIME_MASK;
    const int64_t txToSequenceMasked = txToSequence & nLockTimeMask;
    const CScriptNum nSequenceMasked = nSequence & nLockTimeMask;

    // There are two kinds of nSequence: lock-by-blockheight
    // and lock-by-blocktime, distinguished by whether
    // nSequenceMasked < CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG.
    //
    // We want to compare apples to apples, so fail the script
    // unless the type of nSequenceMasked being tested is the same as
    // the nSequenceMasked in the transaction.
    if (!(
        (txToSequenceMasked <  CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG && nSequenceMasked <  CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) ||
        (txToSequenceMasked >= CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG && nSequenceMasked >= CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG)
    )) {
        return false;
    }

    // Now that we know we're comparing apples-to-apples, the
    // comparison is a simple numeric one.
    if (nSequenceMasked > txToSequenceMasked)
        return false;

    return true;
}

template <class T>
uint32_t GenericTransactionSignatureChecker<T>::GetLockTime() const
{
    return txTo->nLockTime;
}

template <class T>
int32_t GenericTransactionSignatureChecker<T>::GetTxVersion() const
{
    return txTo->nVersion;
}

template <class T>
const std::vector<CTxIn>* GenericTransactionSignatureChecker<T>::GetTxvIn() const
{
    return &(txTo->vin);
}

template <class T>
const std::vector<CTxOut>* GenericTransactionSignatureChecker<T>::GetTxvOut() const
{
    return &(txTo->vout);
}

template <class T>
const PrecomputedTransactionData* GenericTransactionSignatureChecker<T>::GetPrecomputedTransactionData() const
{
    return txdata;
}

template <class T>
uint32_t GenericTransactionSignatureChecker<T>::GetnIn() const
{
    return nIn;
}

template <class T>
bool GenericTransactionSignatureChecker<T>::CheckSimplicity(const valtype& program, const valtype& witness, const rawElementsTapEnv& simplicityRawTap, int64_t budget, ScriptError* serror) const
{
    simplicity_err error;
    elementsTapEnv* simplicityTapEnv = simplicity_elements_mallocTapEnv(&simplicityRawTap);

    assert(txdata->m_simplicity_tx_data);
    assert(simplicityTapEnv);
    if (!simplicity_elements_execSimplicity(&error, 0, txdata->m_simplicity_tx_data.get(), nIn, simplicityTapEnv, txdata->m_hash_genesis_block.data(), 0, budget, 0, program.data(), program.size(), witness.data(), witness.size())) {
        assert(!"simplicity_elements_execSimplicity internal error");
    }
    simplicity_elements_freeTapEnv(simplicityTapEnv);
    switch (error) {
    case SIMPLICITY_NO_ERROR: return set_success(serror);
    case SIMPLICITY_ERR_MALLOC:
    case SIMPLICITY_ERR_NOT_YET_IMPLEMENTED:
        assert(!"simplicity_elements_execSimplicity internal error");
        break;
    case SIMPLICITY_ERR_DATA_OUT_OF_RANGE: return set_error(serror, SCRIPT_ERR_SIMPLICITY_DATA_OUT_OF_RANGE);
    case SIMPLICITY_ERR_DATA_OUT_OF_ORDER: return set_error(serror, SCRIPT_ERR_SIMPLICITY_DATA_OUT_OF_ORDER);
    case SIMPLICITY_ERR_FAIL_CODE: return set_error(serror, SCRIPT_ERR_SIMPLICITY_FAIL_CODE);
    case SIMPLICITY_ERR_RESERVED_CODE: return set_error(serror, SCRIPT_ERR_SIMPLICITY_RESERVED_CODE);
    case SIMPLICITY_ERR_HIDDEN: return set_error(serror, SCRIPT_ERR_SIMPLICITY_HIDDEN);
    case SIMPLICITY_ERR_BITSTREAM_EOF: return set_error(serror, SCRIPT_ERR_SIMPLICITY_BITSTREAM_EOF);
    case SIMPLICITY_ERR_BITSTREAM_TRAILING_BYTES: return set_error(serror, SCRIPT_ERR_SIMPLICITY_BITSTREAM_TRAILING_BYTES);
    case SIMPLICITY_ERR_BITSTREAM_ILLEGAL_PADDING: return set_error(serror, SCRIPT_ERR_SIMPLICITY_BITSTREAM_ILLEGAL_PADDING);
    case SIMPLICITY_ERR_TYPE_INFERENCE_UNIFICATION: return set_error(serror, SCRIPT_ERR_SIMPLICITY_TYPE_INFERENCE_UNIFICATION);
    case SIMPLICITY_ERR_TYPE_INFERENCE_OCCURS_CHECK: return set_error(serror, SCRIPT_ERR_SIMPLICITY_TYPE_INFERENCE_OCCURS_CHECK);
    case SIMPLICITY_ERR_TYPE_INFERENCE_NOT_PROGRAM: return set_error(serror, SCRIPT_ERR_SIMPLICITY_TYPE_INFERENCE_NOT_PROGRAM);
    case SIMPLICITY_ERR_WITNESS_EOF: return set_error(serror, SCRIPT_ERR_SIMPLICITY_WITNESS_EOF);
    case SIMPLICITY_ERR_WITNESS_TRAILING_BYTES: return set_error(serror, SCRIPT_ERR_SIMPLICITY_WITNESS_TRAILING_BYTES);
    case SIMPLICITY_ERR_WITNESS_ILLEGAL_PADDING: return set_error(serror, SCRIPT_ERR_SIMPLICITY_WITNESS_ILLEGAL_PADDING);
    case SIMPLICITY_ERR_UNSHARED_SUBEXPRESSION: return set_error(serror, SCRIPT_ERR_SIMPLICITY_UNSHARED_SUBEXPRESSION);
    case SIMPLICITY_ERR_CMR: return set_error(serror, SCRIPT_ERR_SIMPLICITY_CMR);
    case SIMPLICITY_ERR_EXEC_BUDGET: return set_error(serror, SCRIPT_ERR_SIMPLICITY_EXEC_BUDGET);
    case SIMPLICITY_ERR_EXEC_MEMORY: return set_error(serror, SCRIPT_ERR_SIMPLICITY_EXEC_MEMORY);
    case SIMPLICITY_ERR_EXEC_JET: return set_error(serror, SCRIPT_ERR_SIMPLICITY_EXEC_JET);
    case SIMPLICITY_ERR_EXEC_ASSERT: return set_error(serror, SCRIPT_ERR_SIMPLICITY_EXEC_ASSERT);
    case SIMPLICITY_ERR_ANTIDOS: return set_error(serror, SCRIPT_ERR_SIMPLICITY_ANTIDOS);
    case SIMPLICITY_ERR_HIDDEN_ROOT: return set_error(serror, SCRIPT_ERR_SIMPLICITY_HIDDEN_ROOT);
    case SIMPLICITY_ERR_AMR: return set_error(serror, SCRIPT_ERR_SIMPLICITY_AMR);
    case SIMPLICITY_ERR_OVERWEIGHT: return set_error(serror, SCRIPT_ERR_SIMPLICITY_OVERWEIGHT);
    default: return set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);
    }
}

// explicit instantiation
template class GenericTransactionSignatureChecker<CTransaction>;
template class GenericTransactionSignatureChecker<CMutableTransaction>;

static bool ExecuteWitnessScript(const Span<const valtype>& stack_span, const CScript& exec_script, unsigned int flags, SigVersion sigversion, const BaseSignatureChecker& checker, ScriptExecutionData& execdata, ScriptError* serror)
{
    std::vector<valtype> stack{stack_span.begin(), stack_span.end()};

    if (sigversion == SigVersion::TAPSCRIPT) {
        // OP_SUCCESSx processing overrides everything, including stack element size limits
        CScript::const_iterator pc = exec_script.begin();
        while (pc < exec_script.end()) {
            opcodetype opcode;
            if (!exec_script.GetOp(pc, opcode)) {
                // Note how this condition would not be reached if an unknown OP_SUCCESSx was found
                return set_error(serror, SCRIPT_ERR_BAD_OPCODE);
            }
            // New opcodes will be listed here. May use a different sigversion to modify existing opcodes.
            if (IsOpSuccess(opcode)) {
                if (flags & SCRIPT_VERIFY_DISCOURAGE_OP_SUCCESS) {
                    return set_error(serror, SCRIPT_ERR_DISCOURAGE_OP_SUCCESS);
                }
                return set_success(serror);
            }
        }

        // Tapscript enforces initial stack size limits (altstack is empty here)
        if (stack.size() > MAX_STACK_SIZE) return set_error(serror, SCRIPT_ERR_STACK_SIZE);
    }

    // Disallow stack item size > MAX_SCRIPT_ELEMENT_SIZE in witness stack
    for (const valtype& elem : stack) {
        if (elem.size() > MAX_SCRIPT_ELEMENT_SIZE) return set_error(serror, SCRIPT_ERR_PUSH_SIZE);
    }

    // Run the script interpreter.
    if (!EvalScript(stack, exec_script, flags, checker, sigversion, execdata, serror)) return false;

    // Scripts inside witness implicitly require cleanstack behaviour
    if (stack.size() != 1) return set_error(serror, SCRIPT_ERR_CLEANSTACK);
    if (!CastToBool(stack.back())) return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
    return true;
}

uint256 ComputeTapleafHash(uint8_t leaf_version, const CScript& script)
{
    return (CHashWriter(HASHER_TAPLEAF_ELEMENTS) << leaf_version << script).GetSHA256();
}

uint256 ComputeTaprootMerkleRoot(Span<const unsigned char> control, const uint256& tapleaf_hash)
{
    const int path_len = (control.size() - TAPROOT_CONTROL_BASE_SIZE) / TAPROOT_CONTROL_NODE_SIZE;
    uint256 k = tapleaf_hash;
    for (int i = 0; i < path_len; ++i) {
        CHashWriter ss_branch = CHashWriter{HASHER_TAPBRANCH_ELEMENTS};
        Span node{Span{control}.subspan(TAPROOT_CONTROL_BASE_SIZE + TAPROOT_CONTROL_NODE_SIZE * i, TAPROOT_CONTROL_NODE_SIZE)};
        if (std::lexicographical_compare(k.begin(), k.end(), node.begin(), node.end())) {
            ss_branch << k << node;
        } else {
            ss_branch << node << k;
        }
        k = ss_branch.GetSHA256();
    }
    return k;
}

static bool VerifyTaprootCommitment(const std::vector<unsigned char>& control, const std::vector<unsigned char>& program, const uint256& tapleaf_hash)
{
    assert(control.size() >= TAPROOT_CONTROL_BASE_SIZE);
    assert(program.size() >= uint256::size());
    //! The internal pubkey (x-only, so no Y coordinate parity).
    const XOnlyPubKey p{Span{control}.subspan(1, TAPROOT_CONTROL_BASE_SIZE - 1)};
    //! The output pubkey (taken from the scriptPubKey).
    const XOnlyPubKey q{program};
    // Compute the Merkle root from the leaf and the provided path.
    const uint256 merkle_root = ComputeTaprootMerkleRoot(control, tapleaf_hash);
    // Verify that the output pubkey matches the tweaked internal pubkey, after correcting for parity.
    return q.CheckTapTweak(p, merkle_root, control[0] & 1);
}

static bool VerifyWitnessProgram(const CScriptWitness& witness, int witversion, const std::vector<unsigned char>& program, unsigned int flags, const BaseSignatureChecker& checker, ScriptError* serror, bool is_p2sh)
{
    CScript exec_script; //!< Actually executed script (last stack item in P2WSH; implied P2PKH script in P2WPKH; leaf script in P2TR)
    Span stack{witness.stack};
    ScriptExecutionData execdata;

    if (witversion == 0) {
        if (program.size() == WITNESS_V0_SCRIPTHASH_SIZE) {
            // BIP141 P2WSH: 32-byte witness v0 program (which encodes SHA256(script))
            if (stack.size() == 0) {
                return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY);
            }
            const valtype& script_bytes = SpanPopBack(stack);
            exec_script = CScript(script_bytes.begin(), script_bytes.end());
            uint256 hash_exec_script;
            CSHA256().Write(exec_script.data(), exec_script.size()).Finalize(hash_exec_script.begin());
            if (memcmp(hash_exec_script.begin(), program.data(), 32)) {
                return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);
            }
            return ExecuteWitnessScript(stack, exec_script, flags, SigVersion::WITNESS_V0, checker, execdata, serror);
        } else if (program.size() == WITNESS_V0_KEYHASH_SIZE) {
            // BIP141 P2WPKH: 20-byte witness v0 program (which encodes Hash160(pubkey))
            if (stack.size() != 2) {
                return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH); // 2 items in witness
            }
            exec_script << OP_DUP << OP_HASH160 << program << OP_EQUALVERIFY << OP_CHECKSIG;
            return ExecuteWitnessScript(stack, exec_script, flags, SigVersion::WITNESS_V0, checker, execdata, serror);
        } else {
            return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH);
        }
    } else if (witversion == 1 && program.size() == WITNESS_V1_TAPROOT_SIZE && !is_p2sh) {
        // BIP341 Taproot: 32-byte non-P2SH witness v1 program (which encodes a P2C-tweaked pubkey)
        if (!(flags & SCRIPT_VERIFY_TAPROOT)) return set_success(serror);
        if (stack.size() == 0) return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY);
        if (stack.size() >= 2 && !stack.back().empty() && stack.back()[0] == ANNEX_TAG) {
            // Drop annex (this is non-standard; see IsWitnessStandard)
            const valtype& annex = SpanPopBack(stack);
            execdata.m_annex_hash = (CHashWriter(SER_GETHASH, 0) << annex).GetSHA256();
            execdata.m_annex_present = true;
        } else {
            execdata.m_annex_present = false;
        }
        execdata.m_annex_init = true;
        if (stack.size() == 1) {
            // Key path spending (stack size is 1 after removing optional annex)
            if (!checker.CheckSchnorrSignature(stack.front(), program, SigVersion::TAPROOT, execdata, serror)) {
                return false; // serror is set
            }
            return set_success(serror);
        } else {
            // Script path spending (stack size is >1 after removing optional annex)
            const valtype& control = SpanPopBack(stack);
            const valtype& script_bytes = SpanPopBack(stack);
            exec_script = CScript(script_bytes.begin(), script_bytes.end());
            if (control.size() < TAPROOT_CONTROL_BASE_SIZE || control.size() > TAPROOT_CONTROL_MAX_SIZE || ((control.size() - TAPROOT_CONTROL_BASE_SIZE) % TAPROOT_CONTROL_NODE_SIZE) != 0) {
                return set_error(serror, SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE);
            }
            execdata.m_tapleaf_hash = ComputeTapleafHash(control[0] & TAPROOT_LEAF_MASK, exec_script);
            if (!VerifyTaprootCommitment(control, program, execdata.m_tapleaf_hash)) {
                return set_error(serror, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH);
            }
            execdata.m_tapleaf_hash_init = true;
            if ((control[0] & TAPROOT_LEAF_MASK) == TAPROOT_LEAF_TAPSCRIPT) {
                // Tapscript (leaf version 0xc4)
                execdata.m_validation_weight_left = ::GetSerializeSize(witness.stack, PROTOCOL_VERSION) + VALIDATION_WEIGHT_OFFSET;
                execdata.m_validation_weight_left_init = true;
                return ExecuteWitnessScript(stack, exec_script, flags, SigVersion::TAPSCRIPT, checker, execdata, serror);
            }
            if ((flags & SCRIPT_VERIFY_SIMPLICITY) && (control[0] & TAPROOT_LEAF_MASK) == TAPROOT_LEAF_TAPSIMPLICITY) {
                if (stack.size() != 2 || script_bytes.size() != 32) return set_error(serror, SCRIPT_ERR_SIMPLICITY_WRONG_LENGTH);
                // Tapsimplicity (leaf version 0xbe)
                const valtype& simplicity_program = SpanPopBack(stack);
                const valtype& simplicity_witness = SpanPopBack(stack);
                const int64_t budget = ::GetSerializeSize(witness.stack, PROTOCOL_VERSION) + VALIDATION_WEIGHT_OFFSET;
                rawElementsTapEnv simplicityRawTap;
                simplicityRawTap.controlBlock = control.data();
                simplicityRawTap.pathLen = (control.size() - TAPROOT_CONTROL_BASE_SIZE) / TAPROOT_CONTROL_NODE_SIZE;
                simplicityRawTap.scriptCMR = script_bytes.data();
                return checker.CheckSimplicity(simplicity_program, simplicity_witness, simplicityRawTap, budget, serror);
            }
            if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION) {
                return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION);
            }
            return set_success(serror);
        }
    } else {
        if (flags & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM) {
            return set_error(serror, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM);
        }
        // Other version/size/p2sh combinations return true for future softfork compatibility
        return true;
    }
    // There is intentionally no return statement here, to be able to use "control reaches end of non-void function" warnings to detect gaps in the logic above.
}

bool VerifyScript(const CScript& scriptSig, const CScript& scriptPubKey, const CScriptWitness* witness, unsigned int flags, const BaseSignatureChecker& checker, ScriptError* serror)
{
    static const CScriptWitness emptyWitness;
    if (witness == nullptr) {
        witness = &emptyWitness;
    }
    bool hadWitness = false;

    set_error(serror, SCRIPT_ERR_UNKNOWN_ERROR);

    if ((flags & SCRIPT_VERIFY_SIGPUSHONLY) != 0 && !scriptSig.IsPushOnly()) {
        return set_error(serror, SCRIPT_ERR_SIG_PUSHONLY);
    }

    // scriptSig and scriptPubKey must be evaluated sequentially on the same stack
    // rather than being simply concatenated (see CVE-2010-5141)
    std::vector<std::vector<unsigned char> > stack, stackCopy;
    if (!EvalScript(stack, scriptSig, flags, checker, SigVersion::BASE, serror))
        // serror is set
        return false;
    if (flags & SCRIPT_VERIFY_P2SH)
        stackCopy = stack;
    if (!EvalScript(stack, scriptPubKey, flags, checker, SigVersion::BASE, serror))
        // serror is set
        return false;
    if (stack.empty())
        return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
    if (CastToBool(stack.back()) == false)
        return set_error(serror, SCRIPT_ERR_EVAL_FALSE);

    // Bare witness programs
    int witnessversion;
    std::vector<unsigned char> witnessprogram;
    if (flags & SCRIPT_VERIFY_WITNESS) {
        if (scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
            hadWitness = true;
            if (scriptSig.size() != 0) {
                // The scriptSig must be _exactly_ CScript(), otherwise we reintroduce malleability.
                return set_error(serror, SCRIPT_ERR_WITNESS_MALLEATED);
            }
            if (!VerifyWitnessProgram(*witness, witnessversion, witnessprogram, flags, checker, serror, /* is_p2sh */ false)) {
                return false;
            }
            // Bypass the cleanstack check at the end. The actual stack is obviously not clean
            // for witness programs.
            stack.resize(1);
        }
    }

    // Additional validation for spend-to-script-hash transactions:
    if ((flags & SCRIPT_VERIFY_P2SH) && scriptPubKey.IsPayToScriptHash())
    {
        // scriptSig must be literals-only or validation fails
        if (!scriptSig.IsPushOnly())
            return set_error(serror, SCRIPT_ERR_SIG_PUSHONLY);

        // Restore stack.
        swap(stack, stackCopy);

        // stack cannot be empty here, because if it was the
        // P2SH  HASH <> EQUAL  scriptPubKey would be evaluated with
        // an empty stack and the EvalScript above would return false.
        assert(!stack.empty());

        const valtype& pubKeySerialized = stack.back();
        CScript pubKey2(pubKeySerialized.begin(), pubKeySerialized.end());
        popstack(stack);

        if (!EvalScript(stack, pubKey2, flags, checker, SigVersion::BASE, serror))
            // serror is set
            return false;
        if (stack.empty())
            return set_error(serror, SCRIPT_ERR_EVAL_FALSE);
        if (!CastToBool(stack.back()))
            return set_error(serror, SCRIPT_ERR_EVAL_FALSE);

        // P2SH witness program
        if (flags & SCRIPT_VERIFY_WITNESS) {
            if (pubKey2.IsWitnessProgram(witnessversion, witnessprogram)) {
                hadWitness = true;
                if (scriptSig != CScript() << std::vector<unsigned char>(pubKey2.begin(), pubKey2.end())) {
                    // The scriptSig must be _exactly_ a single push of the redeemScript. Otherwise we
                    // reintroduce malleability.
                    return set_error(serror, SCRIPT_ERR_WITNESS_MALLEATED_P2SH);
                }
                if (!VerifyWitnessProgram(*witness, witnessversion, witnessprogram, flags, checker, serror, /* is_p2sh */ true)) {
                    return false;
                }
                // Bypass the cleanstack check at the end. The actual stack is obviously not clean
                // for witness programs.
                stack.resize(1);
            }
        }
    }

    // The CLEANSTACK check is only performed after potential P2SH evaluation,
    // as the non-P2SH evaluation of a P2SH script will obviously not result in
    // a clean stack (the P2SH inputs remain). The same holds for witness evaluation.
    if ((flags & SCRIPT_VERIFY_CLEANSTACK) != 0) {
        // Disallow CLEANSTACK without P2SH, as otherwise a switch CLEANSTACK->P2SH+CLEANSTACK
        // would be possible, which is not a softfork (and P2SH should be one).
        assert((flags & SCRIPT_VERIFY_P2SH) != 0);
        assert((flags & SCRIPT_VERIFY_WITNESS) != 0);
        if (stack.size() != 1) {
            return set_error(serror, SCRIPT_ERR_CLEANSTACK);
        }
    }

    if (flags & SCRIPT_VERIFY_WITNESS) {
        // We can't check for correct unexpected witness data if P2SH was off, so require
        // that WITNESS implies P2SH. Otherwise, going from WITNESS->P2SH+WITNESS would be
        // possible, which is not a softfork.
        assert((flags & SCRIPT_VERIFY_P2SH) != 0);
        if (!hadWitness && !witness->IsNull()) {
            return set_error(serror, SCRIPT_ERR_WITNESS_UNEXPECTED);
        }
    }

    return set_success(serror);
}

size_t static WitnessSigOps(int witversion, const std::vector<unsigned char>& witprogram, const CScriptWitness& witness)
{
    if (witversion == 0) {
        if (witprogram.size() == WITNESS_V0_KEYHASH_SIZE)
            return 1;

        if (witprogram.size() == WITNESS_V0_SCRIPTHASH_SIZE && witness.stack.size() > 0) {
            CScript subscript(witness.stack.back().begin(), witness.stack.back().end());
            return subscript.GetSigOpCount(true);
        }
    }

    // Future flags may be implemented here.
    return 0;
}

size_t CountWitnessSigOps(const CScript& scriptSig, const CScript& scriptPubKey, const CScriptWitness* witness, unsigned int flags)
{
    static const CScriptWitness witnessEmpty;

    if ((flags & SCRIPT_VERIFY_WITNESS) == 0) {
        return 0;
    }
    assert((flags & SCRIPT_VERIFY_P2SH) != 0);

    int witnessversion;
    std::vector<unsigned char> witnessprogram;
    if (scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
        return WitnessSigOps(witnessversion, witnessprogram, witness ? *witness : witnessEmpty);
    }

    if (scriptPubKey.IsPayToScriptHash() && scriptSig.IsPushOnly()) {
        CScript::const_iterator pc = scriptSig.begin();
        std::vector<unsigned char> data;
        while (pc < scriptSig.end()) {
            opcodetype opcode;
            scriptSig.GetOp(pc, opcode, data);
        }
        CScript subscript(data.begin(), data.end());
        if (subscript.IsWitnessProgram(witnessversion, witnessprogram)) {
            return WitnessSigOps(witnessversion, witnessprogram, witness ? *witness : witnessEmpty);
        }
    }

    return 0;
}
