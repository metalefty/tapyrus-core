// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2019 Chaintope Inc.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/standard.h>

#include <crypto/sha256.h>
#include <pubkey.h>
#include <script/script.h>
#include <util.h>
#include <utilstrencodings.h>
#include <script/script.h>


typedef std::vector<unsigned char> valtype;

bool fAcceptDatacarrier = DEFAULT_ACCEPT_DATACARRIER;
unsigned nMaxDatacarrierBytes = MAX_OP_RETURN_RELAY;

CScriptID::CScriptID(const CScript& in) : uint160(Hash160(in.begin(), in.end())) {}

#ifdef DEBUG
WitnessV0ScriptHash::WitnessV0ScriptHash(const CScript& in)
{
    CSHA256().Write(in.data(), in.size()).Finalize(begin());
}
#endif

const char* GetTxnOutputType(txnouttype t)
{
    switch (t)
    {
    case TX_NONSTANDARD: return "nonstandard";
    case TX_PUBKEY: return "pubkey";
    case TX_PUBKEYHASH: return "pubkeyhash";
    case TX_SCRIPTHASH: return "scripthash";
    case TX_MULTISIG: return "multisig";
    case TX_NULL_DATA: return "nulldata";
    case TX_CUSTOM: return "custom";
    case TX_COLOR_PUBKEYHASH: return "coloredpubkeyhash";
    case TX_COLOR_SCRIPTHASH: return "coloredscripthash";
#ifdef DEBUG
    case TX_WITNESS_V0_KEYHASH: return "witness_v0_keyhash";
    case TX_WITNESS_V0_SCRIPTHASH: return "witness_v0_scripthash";
    case TX_WITNESS_UNKNOWN: return "witness_unknown";
#endif
    }
    return nullptr;
}

static bool MatchPayToPubkey(const CScript& script, valtype& pubkey)
{
    if (script.size() == CPubKey::PUBLIC_KEY_SIZE + 2 && script[0] == CPubKey::PUBLIC_KEY_SIZE && script.back() == OP_CHECKSIG) {
        pubkey = valtype(script.begin() + 1, script.begin() + CPubKey::PUBLIC_KEY_SIZE + 1);
        return CPubKey::ValidSize(pubkey);
    }
    if (script.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE + 2 && script[0] == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE && script.back() == OP_CHECKSIG) {
        pubkey = valtype(script.begin() + 1, script.begin() + CPubKey::COMPRESSED_PUBLIC_KEY_SIZE + 1);
        return CPubKey::ValidSize(pubkey);
    }
    return false;
}

static bool MatchPayToPubkeyHash(const CScript& script, valtype& pubkeyhash)
{
    if (script.size() == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 && script[2] == 20 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        pubkeyhash = valtype(script.begin () + 3, script.begin() + 23);
        return true;
    }
    return false;
}

bool MatchColoredPayToPubkeyHash(const CScript& script, valtype& pubkeyhash, valtype& colorid)
{
    //<COLOR identifier> OP_COLOR OP_DUP OP_HASH160 <H(pubkey)> OP_EQUALVERIFY OP_CHECKSIG
    // <COLOR identifier> : TYPE = 1 byte and 32 byte PAYLOAD
    if (script.size() == 60 && script[0] == 0x21  && (script[1] == 0x01 || script[1] == 0x02 || script[1] == 0x03) && script[34] == OP_COLOR && script[35] == OP_DUP && script[36] == OP_HASH160 && script[37] == 20 && script[58] == OP_EQUALVERIFY && script[59] == OP_CHECKSIG)
    {
        pubkeyhash = valtype(script.begin() + 38, script.begin() + 58);
        colorid = valtype(script.begin() + 1, script.begin() + 34);
        return true;
    }
    return false;
}

bool MatchCustomColoredScript(const CScript& script, valtype& colorid)
{
    //search for colorid in the script
    // pattern: 0x21<33 byte>OP_COLOR
    std::vector<unsigned char> colorId;
    CScript::const_iterator iterColorId1 = std::find(script.begin(), script.end(), 0x21);
    CScript::const_iterator iterOpColor = script.begin();
    opcodetype opcode;
    while (iterOpColor < script.end())
    {
        if (!script.GetOp(iterOpColor, opcode))
            return false;
        if (opcode == OP_COLOR)
            break;
    }

    if(iterOpColor == script.end())
        return false;

    if(iterColorId1 != script.end() && std::distance(iterColorId1, iterOpColor) == 34)
    {
        colorId.assign(iterColorId1 + 1, iterColorId1 + 34);
        return true;
    }
    
    return false;
}

/** Test for "small positive integer" script opcodes - OP_1 through OP_16. */
static constexpr bool IsSmallInteger(opcodetype opcode)
{
    return opcode >= OP_1 && opcode <= OP_16;
}

static bool MatchMultisig(const CScript& script, unsigned int& required, std::vector<valtype>& pubkeys)
{
    opcodetype opcode;
    valtype data;
    CScript::const_iterator it = script.begin();
    if (script.size() < 1 || script.back() != OP_CHECKMULTISIG) return false;

    if (!script.GetOp(it, opcode, data) || !IsSmallInteger(opcode)) return false;
    required = CScript::DecodeOP_N(opcode);
    while (script.GetOp(it, opcode, data) && CPubKey::ValidSize(data)) {
        pubkeys.emplace_back(std::move(data));
    }
    if (!IsSmallInteger(opcode)) return false;
    unsigned int keys = CScript::DecodeOP_N(opcode);
    if (pubkeys.size() != keys || keys < required) return false;
    return (it + 1 == script.end());
}

static bool CheckScriptSyntax(const CScript& script)
{
    std::vector<std::vector<unsigned char> > stack = {{0x00, 0x01},{0x00, 0x01}};

    CScript::const_iterator it = script.begin();
    valtype data;
    opcodetype opcode;
    int nOpCount = 0;

    //some of the opcode checks from EvalScript are performed here on scriptPubkey
    while (it < script.end()) {

        if(!script.GetOp(it, opcode, data))
            return false;

        if (data.size() > MAX_SCRIPT_ELEMENT_SIZE)
            return false;

        if (opcode > OP_16 && ++nOpCount > MAX_OPS_PER_SCRIPT)
            return false;

        if( opcode == OP_CAT ||
            opcode == OP_SUBSTR ||
            opcode == OP_LEFT ||
            opcode == OP_RIGHT ||
            opcode == OP_INVERT ||
            opcode == OP_AND ||
            opcode == OP_OR ||
            opcode == OP_XOR ||
            opcode == OP_2MUL ||
            opcode == OP_2DIV ||
            opcode == OP_MUL ||
            opcode == OP_DIV ||
            opcode == OP_MOD ||
            opcode == OP_LSHIFT ||
            opcode == OP_RSHIFT ||
            opcode == OP_VER ||
            opcode == OP_VERIF ||
            opcode == OP_VERNOTIF ||
            opcode == OP_RESERVED ||
            opcode == OP_RESERVED1 ||
            opcode == OP_RESERVED2 ||
            opcode ==  OP_NOP1 ||
            opcode ==  OP_NOP4 ||
            opcode ==  OP_NOP5 ||
            opcode ==  OP_NOP6 ||
            opcode ==  OP_NOP7 ||
            opcode ==  OP_NOP8 ||
            opcode ==  OP_NOP9 ||
            opcode ==  OP_NOP10)
                return false;
    }
    return true;
}

bool Solver(const CScript& scriptPubKey, txnouttype& typeRet, std::vector<std::vector<unsigned char> >& vSolutionsRet)
{
    vSolutionsRet.clear();

    // Shortcut for pay-to-script-hash, which are more constrained than the other types:
    // it is always OP_HASH160 20 [20 byte hash] OP_EQUAL
    if (scriptPubKey.IsPayToScriptHash())
    {
        typeRet = TX_SCRIPTHASH;
        std::vector<unsigned char> hashBytes(scriptPubKey.begin()+2, scriptPubKey.begin()+22);
        vSolutionsRet.push_back(hashBytes);
        return true;
    }

    if (scriptPubKey.IsColoredPayToScriptHash())
    {
        typeRet = TX_COLOR_SCRIPTHASH;
        std::vector<unsigned char> hashBytes;
        std::vector<unsigned char> colorId;
        hashBytes.assign(scriptPubKey.begin()+37, scriptPubKey.begin()+57);
        colorId.assign(scriptPubKey.begin()+1, scriptPubKey.begin()+34);
        vSolutionsRet.push_back(hashBytes);
        vSolutionsRet.push_back(colorId);
        return true;
    }

    int witnessversion;
    std::vector<unsigned char> witnessprogram;
    if (scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
        typeRet = TX_NONSTANDARD;
        return true;
    }

    // Provably prunable, data-carrying output
    //
    // So long as script passes the IsUnspendable() test and all but the first
    // byte passes the IsPushOnly() test we don't care what exactly is in the
    // script.
    if (scriptPubKey.size() >= 1 && scriptPubKey[0] == OP_RETURN && scriptPubKey.IsPushOnly(scriptPubKey.begin()+1)) {
        typeRet = TX_NULL_DATA;
        return true;
    }

    std::vector<unsigned char> data;
    if (MatchPayToPubkey(scriptPubKey, data)) {
        typeRet = TX_PUBKEY;
        vSolutionsRet.push_back(std::move(data));
        return true;
    }

    if (MatchPayToPubkeyHash(scriptPubKey, data)) {
        typeRet = TX_PUBKEYHASH;
        vSolutionsRet.push_back(std::move(data));
        return true;
    }

    std::vector<unsigned char> colorId;
    if (MatchColoredPayToPubkeyHash(scriptPubKey, data, colorId)) {
        typeRet = TX_COLOR_PUBKEYHASH;
        vSolutionsRet.push_back(std::move(data));
        vSolutionsRet.push_back(std::move(colorId));
        return true;
    }

    unsigned int required;
    std::vector<std::vector<unsigned char>> keys;
    if (MatchMultisig(scriptPubKey, required, keys)) {
        typeRet = TX_MULTISIG;
        vSolutionsRet.push_back({static_cast<unsigned char>(required)}); // safe as required is in range 1..16
        vSolutionsRet.insert(vSolutionsRet.end(), keys.begin(), keys.end());
        vSolutionsRet.push_back({static_cast<unsigned char>(keys.size())}); // safe as size is in range 1..16
        return true;
    }

    if (!CheckScriptSyntax(scriptPubKey)) {
        typeRet = TX_NONSTANDARD;
        vSolutionsRet.clear();
        return false;
    }

    vSolutionsRet.clear();
    typeRet = TX_CUSTOM;
    return true;
}

bool ExtractDestination(const CScript& scriptPubKey, CTxDestination& addressRet)
{
    std::vector<valtype> vSolutions;
    txnouttype whichType;
    if (!Solver(scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        CPubKey pubKey(vSolutions[0]);
        if (!pubKey.IsValid())
            return false;

        addressRet = pubKey.GetID();
        return true;
    }
    else if (whichType == TX_PUBKEYHASH
          || whichType == TX_COLOR_PUBKEYHASH)
    {
        addressRet = CKeyID(uint160(vSolutions[0]));
        return true;
    }
    else if (whichType == TX_SCRIPTHASH
          || whichType == TX_COLOR_SCRIPTHASH)
    {
        addressRet = CScriptID(uint160(vSolutions[0]));
        return true;
    }
#ifdef DEBUG
    else if (whichType == TX_WITNESS_V0_KEYHASH) {
        WitnessV0KeyHash hash;
        std::copy(vSolutions[0].begin(), vSolutions[0].end(), hash.begin());
        addressRet = hash;
        return true;
    } else if (whichType == TX_WITNESS_V0_SCRIPTHASH) {
        WitnessV0ScriptHash hash;
        std::copy(vSolutions[0].begin(), vSolutions[0].end(), hash.begin());
        addressRet = hash;
        return true;
    } else if (whichType == TX_WITNESS_UNKNOWN) {
        WitnessUnknown unk;
        unk.version = vSolutions[0][0];
        std::copy(vSolutions[1].begin(), vSolutions[1].end(), unk.program);
        unk.length = vSolutions[1].size();
        addressRet = unk;
        return true;
    }
#endif
    // Multisig txns have more than one address...
    return false;
}

bool ExtractDestinations(const CScript& scriptPubKey, txnouttype& typeRet, std::vector<CTxDestination>& addressRet, int& nRequiredRet)
{
    addressRet.clear();
    typeRet = TX_NONSTANDARD;
    std::vector<valtype> vSolutions;
    if (!Solver(scriptPubKey, typeRet, vSolutions))
        return false;
    if (typeRet == TX_NULL_DATA){
        // This is data, not addresses
        return false;
    }

    if (typeRet == TX_MULTISIG)
    {
        nRequiredRet = vSolutions.front()[0];
        for (unsigned int i = 1; i < vSolutions.size()-1; i++)
        {
            CPubKey pubKey(vSolutions[i]);
            if (!pubKey.IsValid())
                continue;

            CTxDestination address = pubKey.GetID();
            addressRet.push_back(address);
        }

        if (addressRet.empty())
            return false;
    }
    else
    {
        nRequiredRet = 1;
        CTxDestination address;
        if (!ExtractDestination(scriptPubKey, address))
           return false;
        addressRet.push_back(address);
    }

    return true;
}

namespace
{
class CScriptVisitor : public boost::static_visitor<bool>
{
private:
    CScript *script;
public:
    explicit CScriptVisitor(CScript *scriptin) { script = scriptin; }

    bool operator()(const CNoDestination &dest) const {
        script->clear();
        return false;
    }

    bool operator()(const CKeyID &keyID) const {
        script->clear();
        *script << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
        return true;
    }

    bool operator()(const CScriptID &scriptID) const {
        script->clear();
        *script << OP_HASH160 << ToByteVector(scriptID) << OP_EQUAL;
        return true;
    }
#ifdef DEBUG
    bool operator()(const WitnessV0KeyHash& id) const
    {
        script->clear();
        *script << OP_0 << ToByteVector(id);
        return true;
    }

    bool operator()(const WitnessV0ScriptHash& id) const
    {
        script->clear();
        *script << OP_0 << ToByteVector(id);
        return true;
    }

    bool operator()(const WitnessUnknown& id) const
    {
        script->clear();
        *script << CScript::EncodeOP_N(id.version) << std::vector<unsigned char>(id.program, id.program + id.length);
        return true;
    }
#endif
};
} // namespace

CScript GetScriptForDestination(const CTxDestination& dest)
{
    CScript script;

    boost::apply_visitor(CScriptVisitor(&script), dest);
    return script;
}

CScript GetScriptForRawPubKey(const CPubKey& pubKey)
{
    return CScript() << std::vector<unsigned char>(pubKey.begin(), pubKey.end()) << OP_CHECKSIG;
}

CScript GetScriptForMultisig(int nRequired, const std::vector<CPubKey>& keys)
{
    CScript script;

    script << CScript::EncodeOP_N(nRequired);
    for (const CPubKey& key : keys)
        script << ToByteVector(key);
    script << CScript::EncodeOP_N(keys.size()) << OP_CHECKMULTISIG;
    return script;
}

CScript GetScriptForWitness(const CScript& redeemscript)
{
#ifdef DEBUG
    txnouttype typ;
    std::vector<std::vector<unsigned char> > vSolutions;
    if (Solver(redeemscript, typ, vSolutions)) {
        if (typ == TX_PUBKEY) {
            return GetScriptForDestination(WitnessV0KeyHash(Hash160(vSolutions[0].begin(), vSolutions[0].end())));
        } else if (typ == TX_PUBKEYHASH) {
            return GetScriptForDestination(WitnessV0KeyHash(vSolutions[0]));
        }
    }
    return GetScriptForDestination(WitnessV0ScriptHash(redeemscript));
#else
    return CScript();
#endif
}

bool IsValidDestination(const CTxDestination& dest) {
    return dest.which() != 0;
}
