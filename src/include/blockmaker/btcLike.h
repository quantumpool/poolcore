#include "merkleTree.h"
#include "stratumWork.h"
#include "serialize.h"
#include "loguru.hpp"

namespace BTC {
namespace Script {
  enum {
    OP_0 = 0,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };
}

static inline double getDifficulty(uint32_t bits)
{
    int nShift = (bits >> 24) & 0xff;
    double dDiff =
        (double)0x0000ffff / (double)(bits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

struct CoinbaseTx {
  xmstream Data;
  unsigned ExtraDataOffset;
  unsigned ExtraNonceOffset;
};

struct TxData {
  const char *HexData;
  size_t HexDataSize;
  uint256 TxId;
  uint256 WitnessHash;
};

struct TxTree {
  TxData Data;
  int64_t Fee;
  size_t DependsOn = std::numeric_limits<size_t>::max();
  bool Visited = false;
};

bool addTransaction(TxTree *tree, size_t index, size_t txNumLimit, std::vector<TxData> &result, int64_t *blockReward);
bool transactionChecker(rapidjson::Value::Array transactions, std::vector<TxData> &result);
bool isSegwitEnabled(rapidjson::Value::Array transactions);
void processCoinbaseDevReward(rapidjson::Value &blockTemplate, int64_t *devFee, xmstream &devScriptPubKey);
void processMinerFund(rapidjson::Value &blockTemplate, int64_t *blockReward, int64_t *devFee, xmstream &devScriptPubKey);
bool calculateWitnessCommitment(rapidjson::Value &blockTemplate, bool txFilter, std::vector<TxData> &processedTransactions, xmstream &witnessCommitment, std::string &error);
void collectTransactions(const std::vector<TxData> &processedTransactions, xmstream &txHexData, std::vector<uint256> &merklePath, size_t &txNum);

template<typename Proto>
bool transactionFilter(rapidjson::Value::Array transactions, size_t txNumLimit, std::vector<TxData> &result, int64_t *blockReward, bool sortByHash)
{
  size_t txNum = transactions.Size();
  std::unique_ptr<TxTree[]> txTree(new TxTree[txNum]);

  // Build hashmap txid -> index
  std::unordered_map<uint256, size_t> txidMap;
  for (size_t i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;
    if (!txSrc.HasMember("txid") || !txSrc["txid"].IsString())
      return false;
    if (!txSrc.HasMember("fee") || !txSrc["fee"].IsInt64())
      return false;

    txTree[i].Data.HexData = txSrc["data"].GetString();
    txTree[i].Data.HexDataSize = txSrc["data"].GetStringLength();
    txTree[i].Data.TxId.SetHex(txSrc["txid"].GetString());
    if (txSrc.HasMember("hash"))
      txTree[i].Data.WitnessHash.SetHex(txSrc["hash"].GetString());

    txTree[i].Fee = txSrc["fee"].GetInt64();
    txidMap[txTree[i].Data.TxId] = i;
    *blockReward -= txTree[i].Fee;
  }

  xmstream txBinaryData;
  typename Proto::Transaction tx;
  for (size_t i = 0; i < txNum; i++) {
    rapidjson::Value &txSrc = transactions[i];
    if (!txSrc.HasMember("data") || !txSrc["data"].IsString())
      return false;

    // Convert hex -> binary data
    txBinaryData.reset();
    const char *txHexData = txSrc["data"].GetString();
    size_t txHexSize = txSrc["data"].GetStringLength();
    hex2bin(txHexData, txHexSize, txBinaryData.reserve<uint8_t>(txHexSize/2));

    // Decode BTC transaction
    txBinaryData.seekSet(0);
    BTC::unserialize(txBinaryData, tx);
    if (txBinaryData.eof() || txBinaryData.remaining())
      return false;

    // Iterate txin, found in-block dependencies
    for (const auto &txin: tx.txIn) {
      auto It = txidMap.find(txin.previousOutputHash);
      if (It != txidMap.end())
        txTree[i].DependsOn = It->second;
    }
  }

  for (size_t i = 0; i < txNum; i++) {
    // Add transactions with its dependencies recursively
    if (!addTransaction(txTree.get(), i, txNumLimit, result, blockReward))
      break;
  }

  // TODO: sort by hash (for BCHN, BCHABC)
  if (sortByHash)
    std::sort(result.begin(), result.end(), [](const TxData &l, const TxData &r) {
      // TODO: use binary representation of txid
      return l.TxId.GetHex() < r.TxId.GetHex();
    });

  return true;
}

template<typename Proto, typename TemplateLoaderTy, typename NotifyTy, typename PrepareForSubmitTy>
class WorkTy : public StratumSingleWork {
public:
  WorkTy(int64_t stratumWorkId, uint64_t uniqueWorkId, PoolBackend *backend, size_t backendIdx, const MiningConfig &miningCfg, const std::vector<uint8_t> &miningAddress, const std::string &coinbaseMessage) :
    StratumSingleWork(stratumWorkId, uniqueWorkId, backend, backendIdx, miningCfg) {
    CoinbaseMessage_ = coinbaseMessage;
    Initialized_ = miningAddress.size() == sizeof(typename Proto::AddressTy);
    if (Initialized_)
      memcpy(MiningAddress_.begin(), &miningAddress[0], miningAddress.size());
  }
  virtual void shareHash(void *data) override { *reinterpret_cast<typename Proto::BlockHashTy*>(data) = Header.GetHash(); }
  virtual std::string blockHash(size_t) override { return Header.GetHash().ToString(); }
  virtual double expectedWork(size_t) override { return getDifficulty(Header.nBits); }
  virtual bool ready() override { return Backend_ != nullptr; }

  virtual void buildBlock(size_t, xmstream &blockHexData) override { buildBlockImpl(Header, CBTxWitness_, blockHexData); }

  virtual void mutate() override {
    Header.nTime = static_cast<uint32_t>(time(nullptr));
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, MiningCfg_, true, NotifyMessage_);
  }

  virtual bool checkConsensus(size_t, double *shareDiff) override { return checkConsensusImpl(Header, shareDiff); }

  virtual void buildNotifyMessage(bool resetPreviousWork) override {
    buildNotifyMessageImpl(this, Header, JobVersion, CBTxLegacy_, MerklePath, MiningCfg_, resetPreviousWork, NotifyMessage_);
  }

  virtual bool prepareForSubmit(const CWorkerConfig &workerCfg, const StratumMessage &msg) override {
    return prepareForSubmitImpl(Header, JobVersion, CBTxLegacy_, CBTxWitness_, MerklePath, workerCfg, MiningCfg_, msg);
  }

  virtual bool loadFromTemplate(rapidjson::Value &document, const std::string &ticker, std::string &error) override {
    if (!document.HasMember("result") || !document["result"].IsObject()) {
      error = "no result";
      return false;
    }

    rapidjson::Value &blockTemplate = document["result"];

    // Check fields:
    // height
    // header:
    //   version
    //   previousblockhash
    //   curtime
    //   bits
    // transactions
    if (!blockTemplate.HasMember("height") ||
        !blockTemplate.HasMember("version") ||
        !blockTemplate.HasMember("previousblockhash") ||
        !blockTemplate.HasMember("curtime") ||
        !blockTemplate.HasMember("bits") ||
        !blockTemplate.HasMember("coinbasevalue") ||
        !blockTemplate.HasMember("transactions") || !blockTemplate["transactions"].IsArray()) {
      error = "missing data";
      return false;
    }

    rapidjson::Value &height = blockTemplate["height"];
    rapidjson::Value &version = blockTemplate["version"];
    rapidjson::Value &hashPrevBlock = blockTemplate["previousblockhash"];
    rapidjson::Value &curtime = blockTemplate["curtime"];
    rapidjson::Value &bits = blockTemplate["bits"];
    rapidjson::Value &coinbaseValue = blockTemplate["coinbasevalue"];
    rapidjson::Value::Array transactions = blockTemplate["transactions"].GetArray();
    if (!height.IsUint64() ||
        !version.IsUint() ||
        !hashPrevBlock.IsString() ||
        !curtime.IsUint() ||
        !bits.IsString() ||
        !coinbaseValue.IsInt64()) {
      error = "height or header data invalid format";
      return false;
    }

    Height_ = height.GetUint64();
    BlockReward_ = coinbaseValue.GetInt64();

    // Check segwit enabled (compare txid and hash for all transactions)
    SegwitEnabled = isSegwitEnabled(transactions);

    // Checking/filtering transactions
    bool txFilter = MiningCfg_.TxNumLimit && transactions.Size() > MiningCfg_.TxNumLimit;
    std::vector<TxData> processedTransactions;
    bool needSortByHash = (ticker == "BCHN" || ticker == "BCHABC");
    if (txFilter)
      transactionFilter<Proto>(transactions, MiningCfg_.TxNumLimit, processedTransactions, &BlockReward_, needSortByHash);
    else
      transactionChecker(transactions, processedTransactions);

    // "coinbasedevreward" (FreeCash/FCH)
    processCoinbaseDevReward(blockTemplate, &DevFee, DevScriptPubKey);
    // "minerfund" (BCHA)
    processMinerFund(blockTemplate, &BlockReward_, &DevFee, DevScriptPubKey);

    if (txFilter)
      LOG_F(INFO, " * [txfilter] transactions num %zu -> %zu; coinbase value %" PRIi64 " -> %" PRIi64 "", static_cast<size_t>(transactions.Size()), processedTransactions.size(), coinbaseValue.GetInt64(), BlockReward_);

    // Calculate witness commitment
    if (SegwitEnabled) {
      if (!calculateWitnessCommitment(blockTemplate, txFilter, processedTransactions, WitnessCommitment, error))
        return false;
    }

    // Fill header
    Header.nVersion = version.GetUint();
    Header.hashPrevBlock.SetHex(hashPrevBlock.GetString());
    Header.hashMerkleRoot.SetNull();
    Header.nTime = curtime.GetUint();
    Header.nBits = strtoul(bits.GetString(), nullptr, 16);
    Header.nNonce = 0;
    JobVersion = Header.nVersion;

    // Coinbase
    buildCoinbaseTx(nullptr, 0, MiningCfg_, CBTxLegacy_, CBTxWitness_);

    // Transactions
    collectTransactions(processedTransactions, TxHexData, MerklePath, TxNum_);
    return true;
  }

  virtual double getAbstractProfitValue(size_t, double price, double coeff) override {
    return price * BlockReward_ / getDifficulty(Header.nBits) * coeff;
  }

  virtual bool resetNotRecommended() override { return false; }

public:
  // Implementation

  /// Build & serialize custom coinbase transaction
  void buildCoinbaseTx(void *coinbaseData, size_t coinbaseSize, const MiningConfig &miningCfg, CoinbaseTx &legacy, CoinbaseTx &witness) {
    typename Proto::Transaction coinbaseTx;

    coinbaseTx.version = SegwitEnabled ? 2 : 1;

    // TxIn
    {
      coinbaseTx.txIn.resize(1);
      typename Proto::TxIn &txIn = coinbaseTx.txIn[0];
      txIn.previousOutputHash.SetNull();
      txIn.previousOutputIndex = std::numeric_limits<uint32_t>::max();

      if (SegwitEnabled) {
        // Witness nonce
        // Use default: 0
        txIn.witnessStack.resize(1);
        txIn.witnessStack[0].resize(32);
        memset(txIn.witnessStack[0].data(), 0, 32);
      }

      // scriptsig
      xmstream scriptsig;
      // Height
      BTC::serializeForCoinbase(scriptsig, Height_);
      size_t extraDataOffset = scriptsig.offsetOf();
      // Coinbase extra data
      if (coinbaseData)
        scriptsig.write(coinbaseData, coinbaseSize);
      // Coinbase message
      scriptsig.write(CoinbaseMessage_.data(), CoinbaseMessage_.size());
      // Extra nonce
      legacy.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(false));
      legacy.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(false));
      witness.ExtraNonceOffset = static_cast<unsigned>(scriptsig.offsetOf() + coinbaseTx.getFirstScriptSigOffset(true));
      witness.ExtraDataOffset = static_cast<unsigned>(extraDataOffset + coinbaseTx.getFirstScriptSigOffset(true));
      scriptsig.reserve(miningCfg.FixedExtraNonceSize + miningCfg.MutableExtraNonceSize);

      xvectorFromStream(std::move(scriptsig), txIn.scriptSig);
      txIn.sequence = std::numeric_limits<uint32_t>::max();
    }

    // TxOut
    {
      typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      txOut.value = BlockReward_;

      // pkScript (use single P2PKH)
      txOut.pkScript.resize(sizeof(typename Proto::AddressTy) + 5);
      xmstream p2pkh(txOut.pkScript.data(), txOut.pkScript.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_DUP);
      p2pkh.write<uint8_t>(BTC::Script::OP_HASH160);
      p2pkh.write<uint8_t>(sizeof(typename Proto::AddressTy));
      p2pkh.write(MiningAddress_.begin(), MiningAddress_.size());
      p2pkh.write<uint8_t>(BTC::Script::OP_EQUALVERIFY);
      p2pkh.write<uint8_t>(BTC::Script::OP_CHECKSIG);
    }

    if (DevFee) {
      typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      txOut.value = DevFee;
      txOut.pkScript.resize(DevScriptPubKey.sizeOf());
      memcpy(txOut.pkScript.begin(), DevScriptPubKey.data(), DevScriptPubKey.sizeOf());
    }

    if (SegwitEnabled) {
      typename Proto::TxOut &txOut = coinbaseTx.txOut.emplace_back();
      txOut.value = 0;
      txOut.pkScript.resize(WitnessCommitment.sizeOf());
      memcpy(txOut.pkScript.data(), WitnessCommitment.data(), WitnessCommitment.sizeOf());
    }

    coinbaseTx.lockTime = 0;
    BTC::Io<typename Proto::Transaction>::serialize(legacy.Data, coinbaseTx, false);
    BTC::Io<typename Proto::Transaction>::serialize(witness.Data, coinbaseTx, true);
  }

  static bool checkConsensusImpl(const typename Proto::BlockHeader &header, double *shareDiff) {
    typename Proto::CheckConsensusCtx ctx;
    typename Proto::ChainParams params;
    Proto::checkConsensusInitialize(ctx);
    return Proto::checkConsensus(header, ctx, params, shareDiff);
  }

  static void buildNotifyMessageImpl(StratumWork *source, typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, const std::vector<uint256> &merklePath, const MiningConfig &cfg, bool resetPreviousWork, xmstream &notifyMessage) {
    NotifyTy::build(source, header, asicBoostData, legacy, merklePath, cfg, resetPreviousWork, notifyMessage);
  }

  static bool prepareForSubmitImpl(typename Proto::BlockHeader &header, uint32_t asicBoostData, CoinbaseTx &legacy, CoinbaseTx &witness, const std::vector<uint256> &merklePath, const CWorkerConfig &workerCfg, const MiningConfig &miningCfg, const StratumMessage &msg) {
    return PrepareForSubmitTy::prepare(header, asicBoostData, legacy, witness, merklePath, workerCfg, miningCfg, msg);
  }

  void buildBlockImpl(typename Proto::BlockHeader &header, CoinbaseTx &witness, xmstream &blockHexData) {
    blockHexData.reset();
    {
      // Header
      uint8_t buffer[1024];
      xmstream stream(buffer, sizeof(buffer));
      stream.reset();
      BTC::serialize(stream, header);

      // Transactions count
      BTC::serializeVarSize(stream, TxNum_ + 1);
      bin2hexLowerCase(stream.data(), blockHexData.reserve<char>(stream.sizeOf()*2), stream.sizeOf());
    }

    // Coinbase (witness)
    bin2hexLowerCase(witness.Data.data(), blockHexData.reserve<char>(witness.Data.sizeOf()*2), witness.Data.sizeOf());

    // Transactions
    blockHexData.write(TxHexData.data(), TxHexData.sizeOf());
  }

public:
  // Header
  typename Proto::BlockHeader Header;
  // ASIC boost data
  uint32_t JobVersion;
  // Various block template data
  bool SegwitEnabled = false;
  std::vector<uint256> MerklePath;
  // Coinbase data
  typename Proto::AddressTy MiningAddress_;
  std::string CoinbaseMessage_;
  int64_t DevFee = 0;
  xmstream DevScriptPubKey;
  xmstream WitnessCommitment;
  CoinbaseTx CBTxLegacy_;
  CoinbaseTx CBTxWitness_;
  // Transaction data
  xmstream TxHexData;
};

}
