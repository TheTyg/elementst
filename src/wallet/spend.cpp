// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blind.h> // ELEMENTS: for MAX_RANGEPROOF_SIZE
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <interfaces/chain.h>
#include <issuance.h> // ELEMENTS: for GenerateAssetEntropy and others
#include <policy/policy.h>
#include <rpc/util.h>  // for GetDestinationBlindingKey and IsBlindDestination
#include <script/signingprovider.h>
#include <util/check.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/transaction.h>
#include <wallet/wallet.h>

using interfaces::FoundBlock;

namespace wallet {
static constexpr size_t OUTPUT_GROUP_MAX_ENTRIES{100};

int GetTxSpendSize(const CWallet& wallet, const CWalletTx& wtx, unsigned int out, bool use_max_sig)
{
    return CalculateMaximumSignedInputSize(wtx.tx->vout[out], &wallet, use_max_sig);
}

std::string COutput::ToString(const CWallet& wallet) const
{
    return strprintf("COutput(%s, %d, %d) [%s] [%s]", tx->GetHash().ToString(), i, nDepth, FormatMoney(tx->GetOutputValueOut(wallet, i)), tx->GetOutputAsset(wallet, i).GetHex());
}

// Helper for producing a max-sized low-S low-R signature (eg 71 bytes)
// or a max-sized low-S signature (e.g. 72 bytes) if use_max_sig is true
bool DummySignInput(const SigningProvider& provider, CMutableTransaction& tx, const size_t nIn, const CTxOut& txout, bool use_max_sig)
{
    // Fill in dummy signatures for fee calculation.
    const CScript& scriptPubKey = txout.scriptPubKey;
    SignatureData sigdata;

    if (!ProduceSignature(provider, use_max_sig ? DUMMY_MAXIMUM_SIGNATURE_CREATOR : DUMMY_SIGNATURE_CREATOR, scriptPubKey, sigdata)) {
        return false;
    }
    UpdateTransaction(tx, nIn, sigdata);
    return true;
}

// Helper for producing a bunch of max-sized low-S low-R signatures (eg 71 bytes)
bool CWallet::DummySignTx(CMutableTransaction &txNew, const std::vector<CTxOut> &txouts, const CCoinControl* coin_control) const
{
    // Fill in dummy signatures for fee calculation.
    int nIn = 0;
    for (const auto& txout : txouts)
    {
        CTxIn& txin = txNew.vin[nIn];
        // If weight was provided, fill the input to that weight
        if (coin_control && coin_control->HasInputWeight(txin.prevout)) {
            if (!FillInputToWeight(txNew, nIn, coin_control->GetInputWeight(txin.prevout))) {
                return false;
            }
            nIn++;
            continue;
        }
        // Use max sig if watch only inputs were used or if this particular input is an external input
        // to ensure a sufficient fee is attained for the requested feerate.
        const bool use_max_sig = coin_control && (coin_control->fAllowWatchOnly || coin_control->IsExternalSelected(txin.prevout));
        const std::unique_ptr<SigningProvider> provider = GetSolvingProvider(txout.scriptPubKey);
        if (!provider || !DummySignInput(*provider, txNew, nIn, txout, use_max_sig)) {
            if (!coin_control || !DummySignInput(coin_control->m_external_provider, txNew, nIn, txout, use_max_sig)) {
                return false;
            }
        }

        nIn++;
    }
    return true;
}

bool FillInputToWeight(CMutableTransaction& mtx, size_t nIn, int64_t target_weight)
{
    assert(mtx.vin[nIn].scriptSig.empty());
    assert(mtx.witness.vtxinwit[nIn].scriptWitness.IsNull());

    int64_t txin_weight = GetTransactionInputWeight(CTransaction(mtx), nIn);

    // Do nothing if the weight that should be added is less than the weight that already exists
    if (target_weight < txin_weight) {
        return false;
    }
    if (target_weight == txin_weight) {
        return true;
    }

    // Subtract current txin weight, which should include empty witness stack
    int64_t add_weight = target_weight - txin_weight;
    assert(add_weight > 0);

    // We will want to subtract the size of the Compact Size UInt that will also be serialized.
    // However doing so when the size is near a boundary can result in a problem where it is not
    // possible to have a stack element size and combination to exactly equal a target.
    // To avoid this possibility, if the weight to add is less than 10 bytes greater than
    // a boundary, the size will be split so that 2/3rds will be in one stack element, and
    // the remaining 1/3rd in another. Using 3rds allows us to avoid additional boundaries.
    // 10 bytes is used because that accounts for the maximum size. This does not need to be super precise.
    if ((add_weight >= 253 && add_weight < 263)
        || (add_weight > std::numeric_limits<uint16_t>::max() && add_weight <= std::numeric_limits<uint16_t>::max() + 10)
        || (add_weight > std::numeric_limits<uint32_t>::max() && add_weight <= std::numeric_limits<uint32_t>::max() + 10)) {
        int64_t first_weight = add_weight / 3;
        add_weight -= first_weight;

        first_weight -= GetSizeOfCompactSize(first_weight);
        mtx.witness.vtxinwit[nIn].scriptWitness.stack.emplace(mtx.witness.vtxinwit[nIn].scriptWitness.stack.end(), first_weight, 0);
    }

    add_weight -= GetSizeOfCompactSize(add_weight);
    mtx.witness.vtxinwit[nIn].scriptWitness.stack.emplace(mtx.witness.vtxinwit[nIn].scriptWitness.stack.end(), add_weight, 0);
    assert(GetTransactionInputWeight(CTransaction(mtx), nIn) == target_weight);

    return true;
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const SigningProvider* provider, bool use_max_sig) {
    CMutableTransaction txn;
    txn.vin.push_back(CTxIn(COutPoint()));
    if (!provider || !DummySignInput(*provider, txn, 0, txout, use_max_sig)) {
        return -1;
    }
    return GetVirtualTransactionInputSize(CTransaction(txn));
}

int CalculateMaximumSignedInputSize(const CTxOut& txout, const CWallet* wallet, bool use_max_sig)
{
    const std::unique_ptr<SigningProvider> provider = wallet->GetSolvingProvider(txout.scriptPubKey);
    return CalculateMaximumSignedInputSize(txout, provider.get(), use_max_sig);
}

// Returns pair of vsize and weight
TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const CCoinControl* coin_control) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet)
{
    std::vector<CTxOut> txouts;
    // Look up the inputs. The inputs are either in the wallet, or in coin_control.
    for (const CTxIn& input : tx.vin) {
        const auto mi = wallet->mapWallet.find(input.prevout.hash);
        if (mi != wallet->mapWallet.end()) {
            assert(input.prevout.n < mi->second.tx->vout.size());
            txouts.emplace_back(mi->second.tx->vout[input.prevout.n]);
        } else if (coin_control) {
            CTxOut txout;
            if (!coin_control->GetExternalOutput(input.prevout, txout)) {
                return TxSize{-1, -1};
            }
            txouts.emplace_back(txout);
        } else {
            return TxSize{-1, -1};
        }
    }
    return CalculateMaximumSignedTxSize(tx, wallet, txouts, coin_control);
}

// txouts needs to be in the order of tx.vin
TxSize CalculateMaximumSignedTxSize(const CTransaction &tx, const CWallet *wallet, const std::vector<CTxOut>& txouts, const CCoinControl* coin_control)
{
    CMutableTransaction txNew(tx);
    if (!wallet->DummySignTx(txNew, txouts, coin_control)) {
        return TxSize{-1, -1};
    }
    CTransaction ctx(txNew);
    int64_t vsize = GetVirtualTransactionSize(ctx);
    int64_t weight = GetTransactionWeight(ctx);
    // ELEMENTS: use discounted vsize for CTs if enabled
    if (Params().GetCreateDiscountCT()) {
        vsize = GetDiscountVirtualTransactionSize(ctx);
    }

    return TxSize{vsize, weight};
}

void AvailableCoins(const CWallet& wallet, std::vector<COutput> &vCoins, const CCoinControl *coinControl, const CAmount &nMinimumAmount, const CAmount &nMaximumAmount, const CAmount &nMinimumSumAmount, const uint64_t nMaximumCount, const CAsset* asset_filter) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    vCoins.clear();
    CAmount nTotal = 0;
    // Either the WALLET_FLAG_AVOID_REUSE flag is not set (in which case we always allow), or we default to avoiding, and only in the case where
    // a coin control object is provided, and has the avoid address reuse flag set to false, do we allow already used addresses
    bool allow_used_addresses = !wallet.IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE) || (coinControl && !coinControl->m_avoid_address_reuse);
    const int min_depth = {coinControl ? coinControl->m_min_depth : DEFAULT_MIN_DEPTH};
    const int max_depth = {coinControl ? coinControl->m_max_depth : DEFAULT_MAX_DEPTH};
    const bool only_safe = {coinControl ? !coinControl->m_include_unsafe_inputs : true};

    std::set<uint256> trusted_parents;
    for (const auto& entry : wallet.mapWallet)
    {
        const uint256& wtxid = entry.first;
        const CWalletTx& wtx = entry.second;

        if (wallet.IsTxImmatureCoinBase(wtx))
            continue;

        int nDepth = wallet.GetTxDepthInMainChain(wtx);
        if (nDepth < 0)
            continue;

        // We should not consider coins which aren't at least in our mempool
        // It's possible for these to be conflicted via ancestors which we may never be able to detect
        if (nDepth == 0 && !wtx.InMempool())
            continue;

        bool safeTx = CachedTxIsTrusted(wallet, wtx, trusted_parents);

        // We should not consider coins from transactions that are replacing
        // other transactions.
        //
        // Example: There is a transaction A which is replaced by bumpfee
        // transaction B. In this case, we want to prevent creation of
        // a transaction B' which spends an output of B.
        //
        // Reason: If transaction A were initially confirmed, transactions B
        // and B' would no longer be valid, so the user would have to create
        // a new transaction C to replace B'. However, in the case of a
        // one-block reorg, transactions B' and C might BOTH be accepted,
        // when the user only wanted one of them. Specifically, there could
        // be a 1-block reorg away from the chain where transactions A and C
        // were accepted to another chain where B, B', and C were all
        // accepted.
        if (nDepth == 0 && wtx.mapValue.count("replaces_txid")) {
            safeTx = false;
        }

        // Similarly, we should not consider coins from transactions that
        // have been replaced. In the example above, we would want to prevent
        // creation of a transaction A' spending an output of A, because if
        // transaction B were initially confirmed, conflicting with A and
        // A', we wouldn't want to the user to create a transaction D
        // intending to replace A', but potentially resulting in a scenario
        // where A, A', and D could all be accepted (instead of just B and
        // D, or just A and A' like the user would want).
        if (nDepth == 0 && wtx.mapValue.count("replaced_by_txid")) {
            safeTx = false;
        }

        if (only_safe && !safeTx) {
            continue;
        }

        if (nDepth < min_depth || nDepth > max_depth) {
            continue;
        }

        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            // Only consider selected coins if add_inputs is false
            if (coinControl && !coinControl->m_add_inputs && !coinControl->IsSelected(COutPoint(entry.first, i))) {
                continue;
            }

            CAmount outValue = wtx.GetOutputValueOut(wallet, i);
            CAsset asset = wtx.GetOutputAsset(wallet, i);
            if (asset_filter && asset != *asset_filter) {
                continue;
            }
            if (outValue < nMinimumAmount || (asset == Params().GetConsensus().pegged_asset && outValue > nMaximumAmount)) {
                continue;
            }

            if (coinControl && coinControl->HasSelected() && !coinControl->fAllowOtherInputs && !coinControl->IsSelected(COutPoint(entry.first, i)))
                continue;

            if (wallet.IsLockedCoin(entry.first, i))
                continue;

            if (wallet.IsSpent(wtxid, i))
                continue;

            isminetype mine = wallet.IsMine(wtx.tx->vout[i]);

            if (mine == ISMINE_NO) {
                continue;
            }

            if (!allow_used_addresses && wallet.IsSpentKey(wtxid, i)) {
                continue;
            }

            std::unique_ptr<SigningProvider> provider = wallet.GetSolvingProvider(wtx.tx->vout[i].scriptPubKey);

            bool solvable = provider ? IsSolvable(*provider, wtx.tx->vout[i].scriptPubKey) : false;
            bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && (coinControl && coinControl->fAllowWatchOnly && solvable));

            vCoins.push_back(COutput(wallet, wtx, i, nDepth, spendable, solvable, safeTx, (coinControl && coinControl->fAllowWatchOnly)));

            // Checks the sum amount of all UTXO's.
            if (nMinimumSumAmount != MAX_MONEY) {
                nTotal += outValue;

                if (nTotal >= nMinimumSumAmount) {
                    return;
                }
            }

            // Checks the maximum number of UTXO's.
            if (nMaximumCount > 0 && vCoins.size() >= nMaximumCount) {
                return;
            }
        }
    }
}

CAmountMap GetAvailableBalance(const CWallet& wallet, const CCoinControl* coinControl)
{
    LOCK(wallet.cs_wallet);

    CAmountMap balance;
    std::vector<COutput> vCoins;
    AvailableCoins(wallet, vCoins, coinControl);
    for (const COutput& out : vCoins) {
        if (out.fSpendable) {
            CAmount amt = out.tx->GetOutputValueOut(wallet, out.i);
            if (amt < 0) {
                continue;
            }
            balance[out.tx->GetOutputAsset(wallet, out.i)] += amt;
        }
    }
    return balance;
}

const CTxOut& FindNonChangeParentOutput(const CWallet& wallet, const CTransaction& tx, int output) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    const CTransaction* ptx = &tx;
    int n = output;
    while (OutputIsChange(wallet, ptx->vout[n]) && ptx->vin.size() > 0) {
        const COutPoint& prevout = ptx->vin[0].prevout;
        auto it = wallet.mapWallet.find(prevout.hash);
        if (it == wallet.mapWallet.end() || it->second.tx->vout.size() <= prevout.n ||
            !wallet.IsMine(it->second.tx->vout[prevout.n])) {
            break;
        }
        ptx = it->second.tx.get();
        n = prevout.n;
    }
    return ptx->vout[n];
}

std::map<CTxDestination, std::vector<COutput>> ListCoins(const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);

    std::map<CTxDestination, std::vector<COutput>> result;
    std::vector<COutput> availableCoins;

    AvailableCoins(wallet, availableCoins);

    for (const COutput& coin : availableCoins) {
        CTxDestination address;
        if ((coin.fSpendable || (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && coin.fSolvable)) &&
            ExtractDestination(FindNonChangeParentOutput(wallet, *coin.tx->tx, coin.i).scriptPubKey, address)) {
            result[address].emplace_back(std::move(coin));
        }
    }

    std::vector<COutPoint> lockedCoins;
    wallet.ListLockedCoins(lockedCoins);
    // Include watch-only for LegacyScriptPubKeyMan wallets without private keys
    const bool include_watch_only = wallet.GetLegacyScriptPubKeyMan() && wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    const isminetype is_mine_filter = include_watch_only ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
    for (const COutPoint& output : lockedCoins) {
        auto it = wallet.mapWallet.find(output.hash);
        if (it != wallet.mapWallet.end()) {
            int depth = wallet.GetTxDepthInMainChain(it->second);
            if (depth >= 0 && output.n < it->second.tx->vout.size() &&
                wallet.IsMine(it->second.tx->vout[output.n]) == is_mine_filter
            ) {
                CTxDestination address;
                if (ExtractDestination(FindNonChangeParentOutput(wallet, *it->second.tx, output.n).scriptPubKey, address)) {
                    result[address].emplace_back(
                        wallet, it->second, output.n, depth, true /* spendable */, true /* solvable */, false /* safe */);
                }
            }
        }
    }

    return result;
}

std::vector<OutputGroup> GroupOutputs(const CWallet& wallet, const std::vector<COutput>& outputs, const CoinSelectionParams& coin_sel_params, const CoinEligibilityFilter& filter, bool positive_only)
{
    std::vector<OutputGroup> groups_out;

    if (!coin_sel_params.m_avoid_partial_spends) {
        // Allowing partial spends  means no grouping. Each COutput gets its own OutputGroup.
        for (const COutput& output : outputs) {
            // Skip outputs we cannot spend
            if (!output.fSpendable) continue;

            size_t ancestors, descendants;
            wallet.chain().getTransactionAncestry(output.tx->GetHash(), ancestors, descendants);
            CInputCoin input_coin = output.GetInputCoin(wallet);

            // Make an OutputGroup containing just this output
            OutputGroup group{coin_sel_params};
            group.Insert(input_coin, output.nDepth, CachedTxIsFromMe(wallet, *output.tx, ISMINE_ALL), ancestors, descendants, positive_only);

            // Check the OutputGroup's eligibility. Only add the eligible ones.
            if (positive_only && group.GetSelectionAmount() <= 0) continue;
            if (group.m_outputs.size() > 0 && group.EligibleForSpending(filter)) groups_out.push_back(group);
        }
        return groups_out;
    }

    // We want to combine COutputs that have the same scriptPubKey into single OutputGroups
    // except when there are more than OUTPUT_GROUP_MAX_ENTRIES COutputs grouped in an OutputGroup.
    // To do this, we maintain a map where the key is the scriptPubKey and the value is a vector of OutputGroups.
    // For each COutput, we check if the scriptPubKey is in the map, and if it is, the COutput's CInputCoin is added
    // to the last OutputGroup in the vector for the scriptPubKey. When the last OutputGroup has
    // OUTPUT_GROUP_MAX_ENTRIES CInputCoins, a new OutputGroup is added to the end of the vector.
    std::map<CScript, std::vector<OutputGroup>> spk_to_groups_map;
    for (const auto& output : outputs) {
        // Skip outputs we cannot spend
        if (!output.fSpendable) continue;

        size_t ancestors, descendants;
        wallet.chain().getTransactionAncestry(output.tx->GetHash(), ancestors, descendants);
        CInputCoin input_coin = output.GetInputCoin(wallet);
        CScript spk = input_coin.txout.scriptPubKey;

        std::vector<OutputGroup>& groups = spk_to_groups_map[spk];

        if (groups.size() == 0) {
            // No OutputGroups for this scriptPubKey yet, add one
            groups.emplace_back(coin_sel_params);
        }

        // Get the last OutputGroup in the vector so that we can add the CInputCoin to it
        // A pointer is used here so that group can be reassigned later if it is full.
        OutputGroup* group = &groups.back();

        // Check if this OutputGroup is full. We limit to OUTPUT_GROUP_MAX_ENTRIES when using -avoidpartialspends
        // to avoid surprising users with very high fees.
        if (group->m_outputs.size() >= OUTPUT_GROUP_MAX_ENTRIES) {
            // The last output group is full, add a new group to the vector and use that group for the insertion
            groups.emplace_back(coin_sel_params);
            group = &groups.back();
        }

        // Add the input_coin to group
        group->Insert(input_coin, output.nDepth, CachedTxIsFromMe(wallet, *output.tx, ISMINE_ALL), ancestors, descendants, positive_only);
    }

    // Now we go through the entire map and pull out the OutputGroups
    for (const auto& spk_and_groups_pair: spk_to_groups_map) {
        const std::vector<OutputGroup>& groups_per_spk= spk_and_groups_pair.second;

        // Go through the vector backwards. This allows for the first item we deal with being the partial group.
        for (auto group_it = groups_per_spk.rbegin(); group_it != groups_per_spk.rend(); group_it++) {
            const OutputGroup& group = *group_it;

            // Don't include partial groups if there are full groups too and we don't want partial groups
            if (group_it == groups_per_spk.rbegin() && groups_per_spk.size() > 1 && !filter.m_include_partial_groups) {
                continue;
            }

            // Check the OutputGroup's eligibility. Only add the eligible ones.
            if (positive_only && group.GetSelectionAmount() <= 0) continue;
            if (group.m_outputs.size() > 0 && group.EligibleForSpending(filter)) groups_out.push_back(group);
        }
    }

    return groups_out;
}

std::optional<SelectionResult> AttemptSelection(const CWallet& wallet, const CAmountMap& mapTargetValue, const CoinEligibilityFilter& eligibility_filter, std::vector<COutput> coins,
                               const CoinSelectionParams& coin_selection_params)
{
    // Vector of results. We will choose the best one based on waste.
    // std::vector<std::tuple<CAmount, std::set<CInputCoin>, CAmountMap>> results;
    std::vector<SelectionResult> results;

    // ELEMENTS: BnB only for policy asset?
    if (mapTargetValue.size() == 1) {
        // Note that unlike KnapsackSolver, we do not include the fee for creating a change output as BnB will not create a change output.
        std::vector<OutputGroup> positive_groups = GroupOutputs(wallet, coins, coin_selection_params, eligibility_filter, true /* positive_only */);

        // ELEMENTS:
        CAsset asset = mapTargetValue.begin()->first;
        CAmount nTargetValue = mapTargetValue.begin()->second;
        // Get output groups that only contain this asset.
        std::vector<OutputGroup> asset_groups;
        for (OutputGroup g : positive_groups) {
            bool add = true;
            for (CInputCoin c : g.m_outputs) {
                if (c.asset != asset) {
                    add = false;
                    break;
                }
            }

            if (add) {
                asset_groups.push_back(g);
            }
        }
        // END ELEMENTS

        if (auto bnb_result{SelectCoinsBnB(positive_groups, nTargetValue, coin_selection_params.m_cost_of_change)}) {
            bnb_result->ComputeAndSetWaste(CAmount(0));
            results.push_back(*bnb_result);
        }

        // We include the minimum final change for SRD as we do want to avoid making really small change.
        // KnapsackSolver does not need this because it includes MIN_CHANGE internally.
        const CAmount srd_target = nTargetValue + coin_selection_params.m_change_fee + MIN_FINAL_CHANGE;
        if (auto srd_result{SelectCoinsSRD(positive_groups, srd_target)}) {
            srd_result->ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
            results.push_back(*srd_result);
        }
    }

    // The knapsack solver has some legacy behavior where it will spend dust outputs. We retain this behavior, so don't filter for positive only here.
    std::vector<OutputGroup> all_groups = GroupOutputs(wallet, coins, coin_selection_params, eligibility_filter, false /* positive_only */);
    // While mapTargetValue includes the transaction fees for non-input things, it does not include the fee for creating a change output.
    // So we need to include that for KnapsackSolver as well, as we are expecting to create a change output.
    CAmountMap mapTargetValue_copy = mapTargetValue;
    if (!coin_selection_params.m_subtract_fee_outputs) {
        mapTargetValue_copy[::policyAsset] += coin_selection_params.m_change_fee;
    }

    if (auto knapsack_result{KnapsackSolver(all_groups, mapTargetValue_copy)}) {
         knapsack_result->ComputeAndSetWaste(coin_selection_params.m_cost_of_change);
         results.push_back(*knapsack_result);
    }

    if (results.size() == 0) {
        // No solution found
        return std::nullopt;
    }

    // Choose the result with the least waste
    // If the waste is the same, choose the one which spends more inputs.
    auto& best_result = *std::min_element(results.begin(), results.end());
    return best_result;
}

std::optional<SelectionResult> SelectCoins(const CWallet& wallet, const std::vector<COutput>& vAvailableCoins, const CAmountMap& mapTargetValue, const CCoinControl& coin_control, const CoinSelectionParams& coin_selection_params) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    std::vector<COutput> vCoins(vAvailableCoins);
    CAmountMap value_to_select = mapTargetValue;

    OutputGroup preset_inputs(coin_selection_params);

    // coin control -> return all selected outputs (we want all selected to go into the transaction for sure)
    if (coin_control.HasSelected() && !coin_control.fAllowOtherInputs)
    {
        for (const COutput& out : vCoins)
        {
            if (!out.fSpendable) continue;

            CAmount amt = out.tx->GetOutputValueOut(wallet, out.i);
            if (amt < 0) {
                continue;
            }
            /* Set depth, from_me, ancestors, and descendants to 0 or false as these don't matter for preset inputs as no actual selection is being done.
             * positive_only is set to false because we want to include all preset inputs, even if they are dust.
             */
            preset_inputs.Insert(out.GetInputCoin(wallet), 0, false, 0, 0, false);
        }
        SelectionResult result(mapTargetValue);
        result.AddInput(preset_inputs);
        if (result.GetSelectedValue() < mapTargetValue) return std::nullopt;
        return result;
    }

    // calculate value from preset inputs and store them
    std::set<CInputCoin> setPresetCoins;

    std::vector<COutPoint> vPresetInputs;
    coin_control.ListSelected(vPresetInputs);
    for (const COutPoint& outpoint : vPresetInputs) {
        int input_bytes = -1;
        CTxOut txout;
        std::map<uint256, CWalletTx>::const_iterator it = wallet.mapWallet.find(outpoint.hash);
        CInputCoin coin(outpoint, txout, 0); // dummy initialization
        if (it != wallet.mapWallet.end()) {
            const CWalletTx& wtx = it->second;
            // Clearly invalid input, fail
            if (wtx.tx->vout.size() <= outpoint.n) {
                return std::nullopt;
            }
            // Just to calculate the marginal byte size
            if (GetTxSpendSize(wallet, wtx, outpoint.n, outpoint.n) < 0) {
                continue;
            }
            input_bytes = GetTxSpendSize(wallet, wtx, outpoint.n, false);
            txout = wtx.tx->vout.at(outpoint.n);
            coin = CInputCoin(wallet, &wtx, outpoint.n, input_bytes);
        } else {
            // The input is external. We did not find the tx in mapWallet.
            if (!coin_control.GetExternalOutput(outpoint, txout)) {
                return std::nullopt;
            }
            input_bytes = CalculateMaximumSignedInputSize(txout, &coin_control.m_external_provider, /* use_max_sig */ true);
            // ELEMENTS: one more try to get a signed input size: for pegins,
            //  the outpoint is provided as external data but the information
            //  needed to spend is in the wallet (not the external provider,
            //  as the user is expecting the wallet to remember this information
            //  after they called getpeginaddress). So try estimating size with
            //  the wallet rather than the external provider.
            if (input_bytes == -1) {
                input_bytes = CalculateMaximumSignedInputSize(txout, &wallet, /* use_max_sig */ true);
            }
            if (!txout.nValue.IsExplicit() || !txout.nAsset.IsExplicit()) {
                return std::nullopt; // We can't get its value, so abort
            }
            coin = CInputCoin(outpoint, txout, input_bytes);
        }
        // If available, override calculated size with coin control specified size
        if (coin_control.HasInputWeight(outpoint)) {
            input_bytes = GetVirtualTransactionSize(coin_control.GetInputWeight(outpoint), 0, 0);
            coin = CInputCoin(outpoint, txout, input_bytes);
        }

        if (coin.m_input_bytes == -1) {
            // error = _("Missing solving data for estimating transaction size"); // ELEMENTS
            return std::nullopt; // Not solvable, can't estimate size for fee
        }
        coin.effective_value = coin.value - coin_selection_params.m_effective_feerate.GetFee(coin.m_input_bytes);
        if (coin_selection_params.m_subtract_fee_outputs) {
            value_to_select[coin.asset] -= coin.value;
        } else {
            value_to_select[coin.asset] -= coin.effective_value;
        }
        setPresetCoins.insert(coin);
        /* Set depth, from_me, ancestors, and descendants to 0 or false as don't matter for preset inputs as no actual selection is being done.
         * positive_only is set to false because we want to include all preset inputs, even if they are dust.
         */
        preset_inputs.Insert(coin, 0, false, 0, 0, false);
    }

    // remove preset inputs from vCoins so that Coin Selection doesn't pick them.
    for (std::vector<COutput>::iterator it = vCoins.begin(); it != vCoins.end() && coin_control.HasSelected();)
    {
        if (setPresetCoins.count(it->GetInputCoin(wallet)))
            it = vCoins.erase(it);
        else
            ++it;
    }

    unsigned int limit_ancestor_count = 0;
    unsigned int limit_descendant_count = 0;
    wallet.chain().getPackageLimits(limit_ancestor_count, limit_descendant_count);
    const size_t max_ancestors = (size_t)std::max<int64_t>(1, limit_ancestor_count);
    const size_t max_descendants = (size_t)std::max<int64_t>(1, limit_descendant_count);
    const bool fRejectLongChains = gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS);

    // ELEMENTS: filter coins for assets we are interested in; always keep policyAsset for fees
    for (std::vector<COutput>::iterator it = vCoins.begin(); it != vCoins.end() && coin_control.HasSelected();) {
        CAsset asset = it->GetInputCoin(wallet).asset;
        if (asset != ::policyAsset && mapTargetValue.find(asset) == mapTargetValue.end()) {
            it = vCoins.erase(it);
        } else {
            ++it;
        }
    }

    // form groups from remaining coins; note that preset coins will not
    // automatically have their associated (same address) coins included
    if (coin_control.m_avoid_partial_spends && vCoins.size() > OUTPUT_GROUP_MAX_ENTRIES) {
        // Cases where we have 101+ outputs all pointing to the same destination may result in
        // privacy leaks as they will potentially be deterministically sorted. We solve that by
        // explicitly shuffling the outputs before processing
        Shuffle(vCoins.begin(), vCoins.end(), FastRandomContext());
    }

    // We will have to do coin selection on the difference between the target and the provided values.
    // If value_to_select <= 0 for all asset types, we are done; but unlike in Bitcoin, this may be
    // true for some assets while being false for others. So clear all the "completed" assets out
    // of value_to_select before calling AttemptSelection.
    for (CAmountMap::const_iterator it = value_to_select.begin(); it != value_to_select.end();) {
        if (it->second <= 0) {
            it = value_to_select.erase(it);
        } else {
            ++it;
        }
    }

    // Coin Selection attempts to select inputs from a pool of eligible UTXOs to fund the
    // transaction at a target feerate. If an attempt fails, more attempts may be made using a more
    // permissive CoinEligibilityFilter.
    std::optional<SelectionResult> res = [&] {
        // Pre-selected inputs already cover the target amount.
        if (value_to_select <= CAmountMap{}) return std::make_optional(SelectionResult(mapTargetValue));

        // If possible, fund the transaction with confirmed UTXOs only. Prefer at least six
        // confirmations on outputs received from other wallets and only spend confirmed change.
        if (auto r1{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(1, 6, 0), vCoins, coin_selection_params)}) return r1;
        if (auto r2{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(1, 1, 0), vCoins, coin_selection_params)}) return r2;

        // Fall back to using zero confirmation change (but with as few ancestors in the mempool as
        // possible) if we cannot fund the transaction otherwise.
        if (wallet.m_spend_zero_conf_change) {
            if (auto r3{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, 2), vCoins, coin_selection_params)}) return r3;
            if (auto r4{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, std::min((size_t)4, max_ancestors/3), std::min((size_t)4, max_descendants/3)),
                                   vCoins, coin_selection_params)}) {
                return r4;
            }
            if (auto r5{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, max_ancestors/2, max_descendants/2),
                                   vCoins, coin_selection_params)}) {
                return r5;
            }
            // If partial groups are allowed, relax the requirement of spending OutputGroups (groups
            // of UTXOs sent to the same address, which are obviously controlled by a single wallet)
            // in their entirety.
            if (auto r6{AttemptSelection(wallet, value_to_select, CoinEligibilityFilter(0, 1, max_ancestors-1, max_descendants-1, true /* include_partial_groups */),
                                   vCoins, coin_selection_params)}) {
                return r6;
            }
            // Try with unsafe inputs if they are allowed. This may spend unconfirmed outputs
            // received from other wallets.
            if (coin_control.m_include_unsafe_inputs) {
                if (auto r7{AttemptSelection(wallet, value_to_select,
                    CoinEligibilityFilter(0 /* conf_mine */, 0 /* conf_theirs */, max_ancestors-1, max_descendants-1, true /* include_partial_groups */),
                    vCoins, coin_selection_params)}) {
                    return r7;
                }
            }
            // Try with unlimited ancestors/descendants. The transaction will still need to meet
            // mempool ancestor/descendant policy to be accepted to mempool and broadcasted, but
            // OutputGroups use heuristics that may overestimate ancestor/descendant counts.
            if (!fRejectLongChains) {
                if (auto r8{AttemptSelection(wallet, value_to_select,
                                      CoinEligibilityFilter(0, 1, std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), true /* include_partial_groups */),
                                      vCoins, coin_selection_params)}) {
                    return r8;
                }
            }
        }
        // Coin Selection failed.
        return std::optional<SelectionResult>();
    }();

    if (!res) return std::nullopt;

    // add preset inputs to the total value selected
    // Add preset inputs to result
    res->AddInput(preset_inputs);

    return res;
}

static bool IsCurrentForAntiFeeSniping(interfaces::Chain& chain, const uint256& block_hash)
{
    if (chain.isInitialBlockDownload()) {
        return false;
    }
    constexpr int64_t MAX_ANTI_FEE_SNIPING_TIP_AGE = 8 * 60 * 60; // in seconds
    int64_t block_time;
    CHECK_NONFATAL(chain.findBlock(block_hash, FoundBlock().time(block_time)));
    if (block_time < (GetTime() - MAX_ANTI_FEE_SNIPING_TIP_AGE)) {
        return false;
    }
    return true;
}

/**
 * Return a height-based locktime for new transactions (uses the height of the
 * current chain tip unless we are not synced with the current chain
 */
static uint32_t GetLocktimeForNewTransaction(interfaces::Chain& chain, const uint256& block_hash, int block_height)
{
    uint32_t locktime;
    // Discourage fee sniping.
    //
    // For a large miner the value of the transactions in the best block and
    // the mempool can exceed the cost of deliberately attempting to mine two
    // blocks to orphan the current best block. By setting nLockTime such that
    // only the next block can include the transaction, we discourage this
    // practice as the height restricted and limited blocksize gives miners
    // considering fee sniping fewer options for pulling off this attack.
    //
    // A simple way to think about this is from the wallet's point of view we
    // always want the blockchain to move forward. By setting nLockTime this
    // way we're basically making the statement that we only want this
    // transaction to appear in the next block; we don't want to potentially
    // encourage reorgs by allowing transactions to appear at lower heights
    // than the next block in forks of the best chain.
    //
    // Of course, the subsidy is high enough, and transaction volume low
    // enough, that fee sniping isn't a problem yet, but by implementing a fix
    // now we ensure code won't be written that makes assumptions about
    // nLockTime that preclude a fix later.
    if (IsCurrentForAntiFeeSniping(chain, block_hash)) {
        locktime = block_height;

        // Secondly occasionally randomly pick a nLockTime even further back, so
        // that transactions that are delayed after signing for whatever reason,
        // e.g. high-latency mix networks and some CoinJoin implementations, have
        // better privacy.
        if (GetRandInt(10) == 0)
            locktime = std::max(0, (int)locktime - GetRandInt(100));
    } else {
        // If our chain is lagging behind, we can't discourage fee sniping nor help
        // the privacy of high-latency transactions. To avoid leaking a potentially
        // unique "nLockTime fingerprint", set nLockTime to a constant.
        locktime = 0;
    }
    assert(locktime < LOCKTIME_THRESHOLD);
    return locktime;
}

// Reset all non-global blinding details.
static void resetBlindDetails(BlindDetails* det, bool preserve_output_data = false) {
    det->i_amount_blinds.clear();
    det->i_asset_blinds.clear();
    det->i_assets.clear();
    det->i_amounts.clear();

    det->o_amounts.clear();
    if (!preserve_output_data) {
        det->o_pubkeys.clear();
    }
    det->o_amount_blinds.clear();
    det->o_assets.clear();
    det->o_asset_blinds.clear();

    if (!preserve_output_data) {
        det->num_to_blind = 0;
        det->change_to_blind = 0;
        det->only_recipient_blind_index = -1;
        det->only_change_pos = -1;
    }
}

static bool fillBlindDetails(BlindDetails* det, CWallet* wallet, CMutableTransaction& txNew, std::vector<CInputCoin>& selected_coins, bilingual_str& error) {
    int num_inputs_blinded = 0;

    // Fill in input blinding details
    for (const CInputCoin& coin : selected_coins) {
        det->i_amount_blinds.push_back(coin.bf_value);
        det->i_asset_blinds.push_back(coin.bf_asset);
        det->i_assets.push_back(coin.asset);
        det->i_amounts.push_back(coin.value);
        if (coin.txout.nValue.IsCommitment() || coin.txout.nAsset.IsCommitment()) {
            num_inputs_blinded++;
        }
    }
    // Fill in output blinding details
    for (size_t nOut = 0; nOut < txNew.vout.size(); nOut++) {
        //TODO(CA) consider removing all blind setting before BlindTransaction as they get cleared anyway
        det->o_amount_blinds.push_back(uint256());
        det->o_asset_blinds.push_back(uint256());
        det->o_assets.push_back(txNew.vout[nOut].nAsset.GetAsset());
        det->o_amounts.push_back(txNew.vout[nOut].nValue.GetAmount());
    }

    // There are a few edge-cases of blinding we need to take care of
    //
    // First, if there are blinded inputs but not outputs to blind
    // We need this to go through, even though no privacy is gained.
    if (num_inputs_blinded > 0 &&  det->num_to_blind == 0) {
        // We need to make sure to dupe an asset that is in input set
        //TODO Have blinding do some extremely minimal rangeproof
        CTxOut newTxOut(det->o_assets.back(), 0, CScript() << OP_RETURN);
        CPubKey blind_pub = wallet->GetBlindingPubKey(newTxOut.scriptPubKey); // irrelevant, just needs to be non-null
        newTxOut.nNonce.vchCommitment = std::vector<unsigned char>(blind_pub.begin(), blind_pub.end());
        txNew.vout.push_back(newTxOut);
        det->o_pubkeys.push_back(wallet->GetBlindingPubKey(newTxOut.scriptPubKey));
        det->o_amount_blinds.push_back(uint256());
        det->o_asset_blinds.push_back(uint256());
        det->o_amounts.push_back(0);
        det->o_assets.push_back(det->o_assets.back());
        det->num_to_blind++;
        wallet->WalletLogPrintf("Adding OP_RETURN output to complete blinding since there are %d blinded inputs and no blinded outputs\n", num_inputs_blinded);

        // No blinded inputs, but 1 blinded output
    } else if (num_inputs_blinded == 0 && det->num_to_blind == 1) {
        if (det->change_to_blind == 1) {
            // Only 1 blinded change, unblind the change
            //TODO Split up change instead if possible
            if (det->ignore_blind_failure) {
                det->num_to_blind--;
                det->change_to_blind--;
                txNew.vout[det->only_change_pos].nNonce.SetNull();
                det->o_pubkeys[det->only_change_pos] = CPubKey();
                det->o_amount_blinds[det->only_change_pos] = uint256();
                det->o_asset_blinds[det->only_change_pos] = uint256();
                wallet->WalletLogPrintf("Unblinding change at index %d due to lack of inputs and other outputs being blinded.\n", det->only_change_pos);
            } else {
                error = _("Change output could not be blinded as there are no blinded inputs and no other blinded outputs.");
                return false;
            }
        } else {
            // 1 blinded destination
            // TODO Attempt to get a blinded input, OR add unblinded coin to make blinded change
            assert(det->only_recipient_blind_index != -1);
            if (det->ignore_blind_failure) {
                det->num_to_blind--;
                txNew.vout[det->only_recipient_blind_index].nNonce.SetNull();
                det->o_pubkeys[det->only_recipient_blind_index] = CPubKey();
                det->o_amount_blinds[det->only_recipient_blind_index] = uint256();
                det->o_asset_blinds[det->only_recipient_blind_index] = uint256();
                wallet->WalletLogPrintf("Unblinding single blinded output at index %d due to lack of inputs and other outputs being blinded.\n", det->only_recipient_blind_index);
            } else {
                error = _("Transaction output could not be blinded as there are no blinded inputs and no other blinded outputs.");
                return false;
            }
        }
    }
    // All other combinations should work.
    return true;
}

static bool CreateTransactionInternal(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        CTransactionRef& tx,
        CAmount& nFeeRet,
        int& nChangePosInOut,
        bilingual_str& error,
        const CCoinControl& coin_control,
        FeeCalculation& fee_calc_out,
        bool sign,
        BlindDetails* blind_details,
        const IssuanceDetails* issuance_details) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (blind_details || issuance_details) {
        assert(g_con_elementsmode);
    }

    if (blind_details) {
        // Clear out previous blinding/data info as needed
        resetBlindDetails(blind_details);
    }

    AssertLockHeld(wallet.cs_wallet);

    CMutableTransaction txNew; // The resulting transaction that we make
    txNew.nLockTime = GetLocktimeForNewTransaction(wallet.chain(), wallet.GetLastBlockHash(), wallet.GetLastBlockHeight());

    CoinSelectionParams coin_selection_params; // Parameters for coin selection, init with dummy
    coin_selection_params.m_avoid_partial_spends = coin_control.m_avoid_partial_spends;

    CScript dummy_script = CScript() << 0x00;
    CAmountMap map_recipients_sum;
    // Always assume that we are at least sending policyAsset.
    map_recipients_sum[::policyAsset] = 0;
    std::vector<std::unique_ptr<ReserveDestination>> reservedest;
    // Set the long term feerate estimate to the wallet's consolidate feerate
    coin_selection_params.m_long_term_feerate = wallet.m_consolidate_feerate;
    const OutputType change_type = wallet.TransactionChangeType(coin_control.m_change_type ? *coin_control.m_change_type : wallet.m_default_change_type, vecSend);
    reservedest.emplace_back(new ReserveDestination(&wallet, change_type)); // policy asset

    std::set<CAsset> assets_seen;
    unsigned int outputs_to_subtract_fee_from = 0; // The number of outputs which we are subtracting the fee from
    for (const auto& recipient : vecSend)
    {
        // Pad change keys to cover total possible number of assets
        // One already exists(for policyAsset), so one for each destination
        if (assets_seen.insert(recipient.asset).second) {
            reservedest.emplace_back(new ReserveDestination(&wallet, change_type));
        }

        // Skip over issuance outputs, no need to select those coins
        if (recipient.asset == CAsset(uint256S("1")) || recipient.asset == CAsset(uint256S("2"))) {
            continue;
        }

        map_recipients_sum[recipient.asset] += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount) {
            outputs_to_subtract_fee_from++;
            coin_selection_params.m_subtract_fee_outputs = true;
        }
    }

    // Create change script that will be used if we need change
    // ELEMENTS: A map that keeps track of the change script for each asset and also
    // the index of the reservedest used for that script (-1 if none).
    std::map<CAsset, std::pair<int, CScript>> mapScriptChange;
    // For manually set change, we need to use the blinding pubkey associated
    // with the manually-set address rather than generating one from the wallet
    std::map<CAsset, std::optional<CPubKey>> mapBlindingKeyChange;

    // coin control: send change to custom address
    if (coin_control.destChange.size() > 0) {
        for (const auto& dest : coin_control.destChange) {
            // No need to test we cover all assets.  We produce error for that later.
            mapScriptChange[dest.first] = std::pair<int, CScript>(-1, GetScriptForDestination(dest.second));
            if (IsBlindDestination(dest.second)) {
                mapBlindingKeyChange[dest.first] = GetDestinationBlindingKey(dest.second);
            } else {
                mapBlindingKeyChange[dest.first] = std::nullopt;
            }
        }
    } else { // no coin control: send change to newly generated address
        // Note: We use a new key here to keep it from being obvious which side is the change.
        //  The drawback is that by not reusing a previous key, the change may be lost if a
        //  backup is restored, if the backup doesn't have the new private key for the change.
        //  If we reused the old key, it would be possible to add code to look for and
        //  rediscover unknown transactions that were written with keys of ours to recover
        //  post-backup change.

        // One change script per output asset.
        size_t index = 0;
        for (const auto& value : map_recipients_sum) {
            // Reserve a new key pair from key pool. If it fails, provide a dummy
            // destination in case we don't need change.
            CTxDestination dest;
            bilingual_str dest_err;
            if (index >= reservedest.size() || !reservedest[index]->GetReservedDestination(dest, true, dest_err)) {
                if (dest_err.empty()) {
                    dest_err = _("Please call keypoolrefill first");
                }
                error = _("Transaction needs a change address, but we can't generate it.") + Untranslated(" ") + dest_err;
                // ELEMENTS: We need to put a dummy destination here. Core uses an empty script
                //  but we can't because empty scripts indicate fees (which trigger assertion
                //  failures in `BlindTransaction`). We also set the index to -1, indicating
                //  that this destination is not actually used, and therefore should not be
                //  returned by the `ReturnDestination` loop below.
                mapScriptChange[value.first] = std::pair<int, CScript>(-1, dummy_script);
            } else {
                mapScriptChange[value.first] = std::pair<int, CScript>(index, GetScriptForDestination(dest));
                ++index;
            }
        }

        // Also make sure we have change scripts for the pre-selected inputs.
        std::vector<COutPoint> vPresetInputs;
        coin_control.ListSelected(vPresetInputs);
        for (const COutPoint& presetInput : vPresetInputs) {
            CAsset asset;
            std::map<uint256, CWalletTx>::const_iterator it = wallet.mapWallet.find(presetInput.hash);
            CTxOut txout;
            if (it != wallet.mapWallet.end()) {
                 asset = it->second.GetOutputAsset(wallet, presetInput.n);
            } else if (coin_control.GetExternalOutput(presetInput, txout)) {
                asset = txout.nAsset.GetAsset();
            } else {
                // Ignore this here, will fail more gracefully later.
                continue;
            }

            if (mapScriptChange.find(asset) != mapScriptChange.end()) {
                // This asset already has a change script.
                continue;
            }

            CTxDestination dest;
            bilingual_str dest_err;
            if (index >= reservedest.size() || !reservedest[index]->GetReservedDestination(dest, true, dest_err)) {
                if (dest_err.empty()) {
                    dest_err = _("Keypool ran out, please call keypoolrefill first");
                }
                error = _("Transaction needs a change address, but we can't generate it.") + Untranslated(" ") + dest_err;
                return false;
            }

            CScript scriptChange = GetScriptForDestination(dest);
            // A valid destination implies a change script (and
            // vice-versa). An empty change script will abort later, if the
            // change keypool ran out, but change is required.
            CHECK_NONFATAL(IsValidDestination(dest) != (scriptChange == dummy_script));
            mapScriptChange[asset] = std::pair<int, CScript>(index, scriptChange);
            ++index;
        }
    }
    assert(mapScriptChange.size() > 0);
    CTxOut change_prototype_txout(mapScriptChange.begin()->first, 0, mapScriptChange.begin()->second.second);
    // TODO CA: Set this for each change output
    coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);
    if (g_con_elementsmode) {
        if (blind_details) {
            change_prototype_txout.nAsset.vchCommitment.resize(33);
            change_prototype_txout.nValue.vchCommitment.resize(33);
            change_prototype_txout.nNonce.vchCommitment.resize(33);
            coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);
            coin_selection_params.change_output_size += (MAX_RANGEPROOF_SIZE + DEFAULT_SURJECTIONPROOF_SIZE + WITNESS_SCALE_FACTOR - 1)/WITNESS_SCALE_FACTOR;
        } else {
            change_prototype_txout.nAsset.vchCommitment.resize(33);
            change_prototype_txout.nValue.vchCommitment.resize(9);
            change_prototype_txout.nNonce.vchCommitment.resize(1);
            coin_selection_params.change_output_size = GetSerializeSize(change_prototype_txout);
        }
    }

    // Get size of spending the change output
    int change_spend_size = CalculateMaximumSignedInputSize(change_prototype_txout, &wallet);
    // If the wallet doesn't know how to sign change output, assume p2sh-p2wpkh
    // as lower-bound to allow BnB to do it's thing
    if (change_spend_size == -1) {
        coin_selection_params.change_spend_size = DUMMY_NESTED_P2WPKH_INPUT_SIZE;
    } else {
        coin_selection_params.change_spend_size = (size_t)change_spend_size;
    }

    // Set discard feerate
    coin_selection_params.m_discard_feerate = GetDiscardRate(wallet);

    // Get the fee rate to use effective values in coin selection
    FeeCalculation feeCalc;
    coin_selection_params.m_effective_feerate = GetMinimumFeeRate(wallet, coin_control, &feeCalc);
    // Do not, ever, assume that it's fine to change the fee rate if the user has explicitly
    // provided one
    if (coin_control.m_feerate && coin_selection_params.m_effective_feerate > *coin_control.m_feerate) {
        error = strprintf(_("Fee rate (%s) is lower than the minimum fee rate setting (%s)"), coin_control.m_feerate->ToString(FeeEstimateMode::SAT_VB), coin_selection_params.m_effective_feerate.ToString(FeeEstimateMode::SAT_VB));
        return false;
    }
    if (feeCalc.reason == FeeReason::FALLBACK && !wallet.m_allow_fallback_fee) {
        // eventually allow a fallback fee
        error = _("Fee estimation failed. Fallbackfee is disabled. Wait a few blocks or enable -fallbackfee.");
        return false;
    }

    // Calculate the cost of change
    // Cost of change is the cost of creating the change output + cost of spending the change output in the future.
    // For creating the change output now, we use the effective feerate.
    // For spending the change output in the future, we use the discard feerate for now.
    // So cost of change = (change output size * effective feerate) + (size of spending change output * discard feerate)
    coin_selection_params.m_change_fee = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.change_output_size);
    coin_selection_params.m_cost_of_change = coin_selection_params.m_discard_feerate.GetFee(coin_selection_params.change_spend_size) + coin_selection_params.m_change_fee;

    // vouts to the payees
    if (!coin_selection_params.m_subtract_fee_outputs) {
        coin_selection_params.tx_noinputs_size = 11; // Static vsize overhead + outputs vsize. 4 nVersion, 4 nLocktime, 1 input count, 1 output count, 1 witness overhead (dummy, flag, stack size)
        if (g_con_elementsmode) {
            coin_selection_params.tx_noinputs_size += 46; // fee output: 9 bytes value, 1 byte scriptPubKey, 33 bytes asset, 1 byte nonce, 1 byte each for null rangeproof/surjectionproof
        }
    }
    // ELEMENTS: If we have blinded inputs but no blinded outputs (which, since the wallet
    //  makes an effort to not produce change, is a common case) then we need to add a
    //  dummy output.
    bool may_need_blinded_dummy = !!blind_details;
    for (const auto& recipient : vecSend)
    {
        CTxOut txout(recipient.asset, recipient.nAmount, recipient.scriptPubKey);
        txout.nNonce.vchCommitment = std::vector<unsigned char>(recipient.confidentiality_key.begin(), recipient.confidentiality_key.end());

        // Include the fee cost for outputs.
        if (!coin_selection_params.m_subtract_fee_outputs) {
            coin_selection_params.tx_noinputs_size += ::GetSerializeSize(txout, PROTOCOL_VERSION);
        }

        if (recipient.asset == policyAsset && IsDust(txout, wallet.chain().relayDustFee()))
        {
            error = _("Transaction amount too small");
            return false;
        }
        txNew.vout.push_back(txout);

        // ELEMENTS
        if (blind_details) {
            blind_details->o_pubkeys.push_back(recipient.confidentiality_key);
            if (blind_details->o_pubkeys.back().IsFullyValid()) {
                may_need_blinded_dummy = false;
                blind_details->num_to_blind++;
                blind_details->only_recipient_blind_index = txNew.vout.size()-1;
                if (!coin_selection_params.m_subtract_fee_outputs) {
                    coin_selection_params.tx_noinputs_size += (MAX_RANGEPROOF_SIZE + DEFAULT_SURJECTIONPROOF_SIZE + WITNESS_SCALE_FACTOR - 1)/WITNESS_SCALE_FACTOR;
                }
            }
        }
    }
    if (may_need_blinded_dummy && !coin_selection_params.m_subtract_fee_outputs) {
        // dummy output: 33 bytes value, 2 byte scriptPubKey, 33 bytes asset, 1 byte nonce, 66 bytes dummy rangeproof, 1 byte null surjectionproof
        // FIXME actually, we currently just hand off to BlindTransaction which will put
        //  a full rangeproof and surjectionproof. We should fix this when we overhaul
        //  the blinding logic.
        coin_selection_params.tx_noinputs_size += 70 + 66 +(MAX_RANGEPROOF_SIZE + DEFAULT_SURJECTIONPROOF_SIZE + WITNESS_SCALE_FACTOR - 1)/WITNESS_SCALE_FACTOR;
    }
    // If we are going to issue an asset, add the issuance data to the noinputs_size so that
    // we allocate enough coins for them.
    if (issuance_details) {
        size_t issue_count = 0;
        for (unsigned int i = 0; i < txNew.vout.size(); i++) {
            if (txNew.vout[i].nAsset.IsExplicit() && txNew.vout[i].nAsset.GetAsset() == CAsset(uint256S("1"))) {
                issue_count++;
            } else if (txNew.vout[i].nAsset.IsExplicit() && txNew.vout[i].nAsset.GetAsset() == CAsset(uint256S("2"))) {
                issue_count++;
            }
        }
        if (issue_count > 0) {
            // Allocate space for blinding nonce, entropy, and whichever of nAmount/nInflationKeys is null
            coin_selection_params.tx_noinputs_size += 2 * 32 + 2 * (2 - issue_count);
        }
        // Allocate non-null nAmount/nInflationKeys and rangeproofs
        if (issuance_details->blind_issuance) {
            coin_selection_params.tx_noinputs_size += issue_count * (33 * WITNESS_SCALE_FACTOR + MAX_RANGEPROOF_SIZE + WITNESS_SCALE_FACTOR - 1) / WITNESS_SCALE_FACTOR;
        } else {
            coin_selection_params.tx_noinputs_size += issue_count * 9;
        }
    }

    // Include the fees for things that aren't inputs, excluding the change output
    const CAmount not_input_fees = coin_selection_params.m_effective_feerate.GetFee(coin_selection_params.tx_noinputs_size);
    CAmountMap map_selection_target = map_recipients_sum;
    map_selection_target[policyAsset] += not_input_fees;

    // Get available coins
    std::vector<COutput> vAvailableCoins;
    AvailableCoins(wallet, vAvailableCoins, &coin_control, 1, MAX_MONEY, MAX_MONEY, 0);

    // Choose coins to use
    std::optional<SelectionResult> result = SelectCoins(wallet, vAvailableCoins, /* nTargetValue */ map_selection_target, coin_control, coin_selection_params);
    if (!result) {
        error = _("Insufficient funds");
        return false;
    }

    // If all of our inputs are explicit, we don't need a blinded dummy
    if (may_need_blinded_dummy) {
        may_need_blinded_dummy = false;
        for (const auto& coin : result->GetInputSet()) {
            if (!coin.txout.nValue.IsExplicit()) {
                may_need_blinded_dummy = true;
                break;
            }
        }
    }

    // Always make a change output
    // We will reduce the fee from this change output later, and remove the output if it is too small.
    // ELEMENTS: wrap this all in a loop, set nChangePosInOut specifically for policy asset
    CAmountMap map_change_and_fee = result->GetSelectedValue() - map_recipients_sum;
    // Zero out any non-policy assets which have zero change value
    for (auto it = map_change_and_fee.begin(); it != map_change_and_fee.end(); ) {
        if (it->first != policyAsset && it->second == 0) {
            it = map_change_and_fee.erase(it);
        } else {
            ++it;
        }
    }

    // Uniformly randomly place change outputs for all assets, except that the policy-asset
    // change may have a fixed position.
    std::vector<std::optional<CAsset>> change_pos{txNew.vout.size() + map_change_and_fee.size()};
    if (nChangePosInOut == -1) {
       // randomly set policyasset change position
    } else if ((unsigned int)nChangePosInOut >= change_pos.size()) {
        error = _("Transaction change output index out of range");
        return false;
    } else {
        change_pos[nChangePosInOut] = policyAsset;
    }

    for (const auto& asset_change_and_fee : map_change_and_fee) {
        // No need to randomly set the policyAsset change if has been set manually
        if (nChangePosInOut >= 0 && asset_change_and_fee.first == policyAsset) {
            continue;
        }

        int index;
        do {
            index = GetRandInt(change_pos.size());
        } while (change_pos[index]);

        change_pos[index] = asset_change_and_fee.first;
        if (asset_change_and_fee.first == policyAsset) {
            nChangePosInOut = index;
        }
    }

    // Create all the change outputs in their respective places, inserting them
    // in increasing order so that none of them affect each others' indices
    for (unsigned int i = 0; i < change_pos.size(); i++) {
        if (!change_pos[i]) {
            continue;
        }

        const CAsset& asset = *change_pos[i];
        const CAmount& change_and_fee = map_change_and_fee.at(asset);

        assert(change_and_fee >= 0);

        const std::map<CAsset, std::pair<int, CScript>>::const_iterator itScript = mapScriptChange.find(asset);
        if (itScript == mapScriptChange.end()) {
            error = Untranslated(strprintf("No change destination provided for asset %s", asset.GetHex()));
            return false;
        }
        CTxOut newTxOut(asset, change_and_fee, itScript->second.second);

        if (blind_details) {
            std::optional<CPubKey> blind_pub = std::nullopt;
            // We cannot blind zero-valued outputs, and anyway they will be dropped
            // later in this function during the dust check
            if (change_and_fee > 0) {
                const auto itBlindingKey = mapBlindingKeyChange.find(asset);
                if (itBlindingKey != mapBlindingKeyChange.end()) {
                    // If the change output was specified, use the blinding key that
                    // came with the specified address (if any)
                    blind_pub = itBlindingKey->second;
                } else {
                    // Otherwise, we generated it from our own wallet, so get the
                    // blinding key from our own wallet.
                    blind_pub = wallet.GetBlindingPubKey(itScript->second.second);
                }
            } else {
                assert(asset == policyAsset);
            }

            if (blind_pub) {
                blind_details->o_pubkeys.insert(blind_details->o_pubkeys.begin() + i, *blind_pub);
                assert(blind_pub->IsFullyValid());

                blind_details->num_to_blind++;
                blind_details->change_to_blind++;
                blind_details->only_change_pos = i;
                // Place the blinding pubkey here in case of fundraw calls
                newTxOut.nNonce.vchCommitment = std::vector<unsigned char>(blind_pub->begin(), blind_pub->end());
            } else {
                blind_details->o_pubkeys.insert(blind_details->o_pubkeys.begin() + i, CPubKey());
            }
        }
        // Insert change output
        txNew.vout.insert(txNew.vout.begin() + i, newTxOut);
    }

    // Add fee output.
    if (g_con_elementsmode) {
        // only create fee output if non-zero fee
        if (coin_selection_params.m_effective_feerate > CFeeRate()) {
            CTxOut fee(::policyAsset, 0, CScript());
            assert(fee.IsFee());
            txNew.vout.push_back(fee);
            if (blind_details) {
                blind_details->o_pubkeys.push_back(CPubKey());
            }
        }
    }
    assert(nChangePosInOut != -1);
    auto change_position = txNew.vout.begin() + nChangePosInOut;
    // end ELEMENTS

    // Set token input if reissuing
    int reissuance_index = -1;
    uint256 token_blinding;

    // Elements: Shuffle here to preserve random ordering for surjection proofs
    // selected_coins = std::vector<CInputCoin>(setCoins.begin(), setCoins.end());
    // Shuffle(selected_coins.begin(), selected_coins.end(), FastRandomContext());
    // Shuffle selected coins and fill in final vin
    std::vector<CInputCoin> selected_coins = result->GetShuffledInputVector();

    // Note how the sequence number is set to non-maxint so that
    // the nLockTime set above actually works.
    //
    // BIP125 defines opt-in RBF as any nSequence < maxint-1, so
    // we use the highest possible value in that range (maxint-2)
    // to avoid conflicting with other possible uses of nSequence,
    // and in the spirit of "smallest possible change from prior
    // behavior."
    const uint32_t nSequence{coin_control.m_signal_bip125_rbf.value_or(wallet.m_signal_rbf) ? MAX_BIP125_RBF_SEQUENCE : CTxIn::MAX_SEQUENCE_NONFINAL};
    for (const auto& coin : selected_coins) {
        txNew.vin.push_back(CTxIn(coin.outpoint, CScript(), nSequence));

        if (issuance_details && coin.asset == issuance_details->reissuance_token) {
            reissuance_index = txNew.vin.size() - 1;
            token_blinding = coin.bf_asset;
        }
    }

    // ELEMENTS add issuance details and blinding details
    std::vector<CKey> issuance_asset_keys;
    std::vector<CKey> issuance_token_keys;
    if (issuance_details) {
        // Fill in issuances now that inputs are set
        assert(txNew.vin.size() > 0);
        int asset_index = -1;
        int token_index = -1;
        for (unsigned int i = 0; i < txNew.vout.size(); i++) {
            if (txNew.vout[i].nAsset.IsExplicit() && txNew.vout[i].nAsset.GetAsset() == CAsset(uint256S("1"))) {
                asset_index = i;
            } else if (txNew.vout[i].nAsset.IsExplicit() && txNew.vout[i].nAsset.GetAsset() == CAsset(uint256S("2"))) {
                token_index = i;
            }
        }
        // Initial issuance request
        if (issuance_details->reissuance_asset.IsNull() && issuance_details->reissuance_token.IsNull() && (asset_index != -1 || token_index != -1)) {
            uint256 entropy;
            CAsset asset;
            CAsset token;
            // Initial issuance always uses vin[0]
            GenerateAssetEntropy(entropy, txNew.vin[0].prevout, issuance_details->contract_hash);
            CalculateAsset(asset, entropy);
            CalculateReissuanceToken(token, entropy, issuance_details->blind_issuance);
            CScript blindingScript(CScript() << OP_RETURN << std::vector<unsigned char>(txNew.vin[0].prevout.hash.begin(), txNew.vin[0].prevout.hash.end()) << txNew.vin[0].prevout.n);
            txNew.vin[0].assetIssuance.assetEntropy = issuance_details->contract_hash;
            // We're making asset outputs, fill out asset type and issuance input
            if (asset_index != -1) {
                txNew.vin[0].assetIssuance.nAmount = txNew.vout[asset_index].nValue;

                txNew.vout[asset_index].nAsset = asset;
                if (issuance_details->blind_issuance && blind_details) {
                    issuance_asset_keys.push_back(wallet.GetBlindingKey(&blindingScript));
                    blind_details->num_to_blind++;
                }
            }
            // We're making reissuance token outputs
            if (token_index != -1) {
                txNew.vin[0].assetIssuance.nInflationKeys = txNew.vout[token_index].nValue;
                txNew.vout[token_index].nAsset = token;
                if (issuance_details->blind_issuance && blind_details) {
                    issuance_token_keys.push_back(wallet.GetBlindingKey(&blindingScript));
                    blind_details->num_to_blind++;

                    // If we're blinding a token issuance and no assets, we must make
                    // the asset issuance a blinded commitment to 0
                    if (asset_index == -1) {
                        txNew.vin[0].assetIssuance.nAmount = 0;
                        issuance_asset_keys.push_back(wallet.GetBlindingKey(&blindingScript));
                        blind_details->num_to_blind++;
                    }
                }
            }
        // Asset being reissued with explicitly named asset/token
        } else if (asset_index != -1) {
            assert(reissuance_index != -1);
            // Fill in output with issuance
            txNew.vout[asset_index].nAsset = issuance_details->reissuance_asset;

            // Fill in issuance
            // Blinding revealing underlying asset
            txNew.vin[reissuance_index].assetIssuance.assetBlindingNonce = token_blinding;
            txNew.vin[reissuance_index].assetIssuance.assetEntropy = issuance_details->entropy;
            txNew.vin[reissuance_index].assetIssuance.nAmount = txNew.vout[asset_index].nValue;

            // If blinded token derivation, blind the issuance
            CAsset temp_token;
            CalculateReissuanceToken(temp_token, issuance_details->entropy, true);
            if (temp_token == issuance_details->reissuance_token && blind_details) {
            CScript blindingScript(CScript() << OP_RETURN << std::vector<unsigned char>(txNew.vin[reissuance_index].prevout.hash.begin(), txNew.vin[reissuance_index].prevout.hash.end()) << txNew.vin[reissuance_index].prevout.n);
                issuance_asset_keys.resize(reissuance_index);
                issuance_asset_keys.push_back(wallet.GetBlindingKey(&blindingScript));
                blind_details->num_to_blind++;
            }
        }
    }

    // Do "initial blinding" for fee estimation purposes
    TxSize tx_sizes;
    CMutableTransaction tx_blinded = txNew;
    if (blind_details) {
        if (!fillBlindDetails(blind_details, &wallet, tx_blinded, selected_coins, error)) {
            return false;
        }
        txNew = tx_blinded; // sigh, `fillBlindDetails` may have modified txNew
        // Update the change position to the new tx
        change_position = txNew.vout.begin() + nChangePosInOut;
        int ret = BlindTransaction(blind_details->i_amount_blinds, blind_details->i_asset_blinds, blind_details->i_assets, blind_details->i_amounts, blind_details->o_amount_blinds, blind_details->o_asset_blinds, blind_details->o_pubkeys, issuance_asset_keys, issuance_token_keys, tx_blinded);
        assert(ret != -1);
        if (ret != blind_details->num_to_blind) {
            error = _("Unable to blind the transaction properly. This should not happen.");
            return false;
        }

        tx_sizes = CalculateMaximumSignedTxSize(CTransaction(tx_blinded), &wallet, &coin_control);
    } else {
        tx_sizes = CalculateMaximumSignedTxSize(CTransaction(txNew), &wallet, &coin_control);
    }
    // end ELEMENTS

    // Calculate the transaction fee
    int nBytes = tx_sizes.vsize;
    if (nBytes == -1) {
        error = _("Missing solving data for estimating transaction size");
        return false;
    }
    nFeeRet = coin_selection_params.m_effective_feerate.GetFee(nBytes);

    // Subtract fee from the change output if not subtracting it from recipient outputs
    CAmount fee_needed = nFeeRet;
    if (!coin_selection_params.m_subtract_fee_outputs) {
        change_position->nValue = change_position->nValue.GetAmount() - fee_needed;
    }

    // We want to drop the change to fees if:
    // 1. The change output would be dust
    // 2. The change is within the (almost) exact match window, i.e. it is less than or equal to the cost of the change output (cost_of_change)
    CAmount change_amount = change_position->nValue.GetAmount();
    if (IsDust(*change_position, coin_selection_params.m_discard_feerate) || change_amount <= coin_selection_params.m_cost_of_change)
    {
        bool was_blinded = blind_details && blind_details->o_pubkeys[nChangePosInOut].IsValid();

        // If the change was blinded, and was the only blinded output, we cannot drop it
        // without causing the transaction to fail to balance. So keep it, and merely
        // zero it out.
        if (was_blinded && blind_details->num_to_blind == 1) {
            assert (may_need_blinded_dummy);
            change_position->scriptPubKey = CScript() << OP_RETURN;
            change_position->nValue = 0;
        } else {
            txNew.vout.erase(change_position);

            change_pos[nChangePosInOut] = std::nullopt;
            tx_blinded.vout.erase(tx_blinded.vout.begin() + nChangePosInOut);
            if (tx_blinded.witness.vtxoutwit.size() > (unsigned) nChangePosInOut) {
                tx_blinded.witness.vtxoutwit.erase(tx_blinded.witness.vtxoutwit.begin() + nChangePosInOut);
            }
            if (blind_details) {

                blind_details->o_amounts.erase(blind_details->o_amounts.begin() + nChangePosInOut);
                blind_details->o_assets.erase(blind_details->o_assets.begin() + nChangePosInOut);
                blind_details->o_pubkeys.erase(blind_details->o_pubkeys.begin() + nChangePosInOut);
                // If change_amount == 0, we did not increment num_to_blind initially
                // and therefore do not need to decrement it here.
                if (was_blinded) {
                    blind_details->num_to_blind--;
                    blind_details->change_to_blind--;

                    // FIXME: If we drop the change *and* this means we have only one
                    //  blinded output *and* we have no blinded inputs, then this puts
                    //  us in a situation where BlindTransaction will fail. This is
                    //  prevented in fillBlindDetails, which adds an OP_RETURN output
                    //  to handle this case. So do this ludicrous hack to accomplish
                    //  this. This whole lump of un-followable-logic needs to be replaced
                    //  by a complete rewriting of the wallet blinding logic.
                    if (blind_details->num_to_blind < 2) {
                        resetBlindDetails(blind_details, true /* don't wipe output data */);
                        if (!fillBlindDetails(blind_details, &wallet, txNew, selected_coins, error)) {
                            return false;
                        }
                    }
                }
            }
        }
        change_amount = 0;
        nChangePosInOut = -1;

        // Because we have dropped this change, the tx size and required fee will be different, so let's recalculate those
        tx_sizes = CalculateMaximumSignedTxSize(CTransaction(tx_blinded), &wallet, &coin_control);
        nBytes = tx_sizes.vsize;
        fee_needed = coin_selection_params.m_effective_feerate.GetFee(nBytes);
    }

    // The only time that fee_needed should be less than the amount available for fees (in change_and_fee - change_amount) is when
    // we are subtracting the fee from the outputs. If this occurs at any other time, it is a bug.
    if (!coin_selection_params.m_subtract_fee_outputs && fee_needed > map_change_and_fee.at(policyAsset) - change_amount) {
        wallet.WalletLogPrintf("ERROR: not enough coins to cover for fee (needed: %d, total: %d, change: %d)\n",
            fee_needed, map_change_and_fee.at(policyAsset), change_amount);
        error = _("Could not cover fee");
        return false;
    }

    // Update nFeeRet in case fee_needed changed due to dropping the change output
    if (fee_needed <= map_change_and_fee.at(policyAsset) - change_amount) {
        nFeeRet = map_change_and_fee.at(policyAsset) - change_amount;
    }

    // Reduce output values for subtractFeeFromAmount
    if (coin_selection_params.m_subtract_fee_outputs) {
        CAmount to_reduce = fee_needed + change_amount - map_change_and_fee.at(policyAsset);
        int i = 0;
        bool fFirst = true;
        for (const auto& recipient : vecSend)
        {
            if (i == nChangePosInOut) {
                ++i;
            }
            CTxOut& txout = txNew.vout[i];

            if (recipient.fSubtractFeeFromAmount)
            {
                CAmount value = txout.nValue.GetAmount();
                if (recipient.asset != policyAsset) {
                    error = Untranslated(strprintf("Wallet does not support more than one type of fee at a time, therefore can not subtract fee from address amount, which is of a different asset id. fee asset: %s recipient asset: %s", policyAsset.GetHex(), recipient.asset.GetHex()));
                    return false;
                }

                value -= to_reduce / outputs_to_subtract_fee_from; // Subtract fee equally from each selected recipient

                if (fFirst) // first receiver pays the remainder not divisible by output count
                {
                    fFirst = false;
                    value -= to_reduce % outputs_to_subtract_fee_from;
                }

                // Error if this output is reduced to be below dust
                if (IsDust(txout, wallet.chain().relayDustFee())) {
                    if (value < 0) {
                        error = _("The transaction amount is too small to pay the fee");
                    } else {
                        error = _("The transaction amount is too small to send after the fee has been deducted");
                    }
                    return false;
                }

                txout.nValue = value;
            }
            ++i;
        }
        nFeeRet = fee_needed;
    }

    // ELEMENTS: Give up if change keypool ran out and change is required
    for (const auto& maybe_change_asset : change_pos) {
        if (maybe_change_asset) {
            auto used = mapScriptChange.extract(*maybe_change_asset);
            if (used.mapped().second == dummy_script) {
                return false;
            }
        }
    }

    // ELEMENTS update fee output
    if (g_con_elementsmode) {
        for (auto& txout : txNew.vout) {
            if (txout.IsFee()) {
                txout.nValue = nFeeRet;
                break;
            }
        }
    }

    // ELEMENTS do actual blinding
    if (blind_details) {
        // Print blinded transaction info before we possibly blow it away when !sign.
        std::string summary = "CreateTransaction created blinded transaction:\nIN: ";
        for (unsigned int i = 0; i < selected_coins.size(); ++i) {
            if (i > 0) {
                summary += "    ";
            }
            summary += strprintf("#%d: %s [%s] (%s [%s])\n", i,
                selected_coins[i].value,
                selected_coins[i].txout.nValue.IsExplicit() ? "explicit" : "blinded",
                selected_coins[i].asset.GetHex(),
                selected_coins[i].txout.nAsset.IsExplicit() ? "explicit" : "blinded"
            );
        }
        summary += "OUT: ";
        for (unsigned int i = 0; i < txNew.vout.size(); ++i) {
            if (i > 0) {
                summary += "     ";
            }
            const CTxOut& unblinded = txNew.vout[i];
            summary += strprintf("#%d: %s%s [%s] (%s [%s])\n", i,
                txNew.vout[i].IsFee() ? "[fee] " : "",
                unblinded.nValue.GetAmount(),
                blind_details->o_pubkeys[i].IsValid() ? "blinded" : "explicit",
                unblinded.nAsset.GetAsset().GetHex(),
                blind_details->o_pubkeys[i].IsValid() ? "blinded" : "explicit"
            );
        }
        wallet.WalletLogPrintf(summary+"\n");

        // Wipe output blinding factors and start over
        blind_details->o_amount_blinds.clear();
        blind_details->o_asset_blinds.clear();
        for (unsigned int i = 0; i < txNew.vout.size(); i++) {
            blind_details->o_amounts[i] = txNew.vout[i].nValue.GetAmount();
            assert(blind_details->o_assets[i] == txNew.vout[i].nAsset.GetAsset());
        }

        if (sign) {
            int ret = BlindTransaction(blind_details->i_amount_blinds, blind_details->i_asset_blinds, blind_details->i_assets, blind_details->i_amounts, blind_details->o_amount_blinds, blind_details->o_asset_blinds,  blind_details->o_pubkeys, issuance_asset_keys, issuance_token_keys, txNew);
            assert(ret != -1);
            if (ret != blind_details->num_to_blind) {
                wallet.WalletLogPrintf("ERROR: tried to blind %d outputs but only blinded %d\n", (int) blind_details->num_to_blind, (int) ret);
                error = _("Unable to blind the transaction properly. This should not happen.");
                return false;
            }
        }
    }

    // Release any change keys that we didn't use.
    for (const auto& it : mapScriptChange) {
        int index = it.second.first;
        if (index < 0) {
            continue;
        }

        reservedest[index]->ReturnDestination();
    }


    if (sign) {
        if (!wallet.SignTransaction(txNew)) {
            error = _("Signing transaction failed");
            return false;
        }
    }

    // Normalize the witness in case it is not serialized before mempool
    if (!txNew.HasWitness()) {
        txNew.witness.SetNull();
    }

    // Return the constructed transaction data.
    tx = MakeTransactionRef(std::move(txNew));

    // Limit size
    if ((sign && GetTransactionWeight(*tx) > MAX_STANDARD_TX_WEIGHT) ||
        (!sign && tx_sizes.weight > MAX_STANDARD_TX_WEIGHT))
    {
        error = _("Transaction too large");
        return false;
    }

    if (nFeeRet > wallet.m_default_max_tx_fee) {
        error = TransactionErrorString(TransactionError::MAX_FEE_EXCEEDED);
        return false;
    }

    if (gArgs.GetBoolArg("-walletrejectlongchains", DEFAULT_WALLET_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the mempool's chain limits
        if (!wallet.chain().checkChainLimits(tx)) {
            error = _("Transaction has too long of a mempool chain");
            return false;
        }
    }

    // Before we return success, we assume any change key will be used to prevent
    // accidental re-use.
    for (auto& reservedest_ : reservedest) {
        reservedest_->KeepDestination();
    }
    fee_calc_out = feeCalc;

    wallet.WalletLogPrintf("Fee Calculation: Fee:%d Bytes:%u Tgt:%d (requested %d) Reason:\"%s\" Decay %.5f: Estimation: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out) Fail: (%g - %g) %.2f%% %.1f/(%.1f %d mem %.1f out)\n",
              nFeeRet, nBytes, feeCalc.returnedTarget, feeCalc.desiredTarget, StringForFeeReason(feeCalc.reason), feeCalc.est.decay,
              feeCalc.est.pass.start, feeCalc.est.pass.end,
              (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) > 0.0 ? 100 * feeCalc.est.pass.withinTarget / (feeCalc.est.pass.totalConfirmed + feeCalc.est.pass.inMempool + feeCalc.est.pass.leftMempool) : 0.0,
              feeCalc.est.pass.withinTarget, feeCalc.est.pass.totalConfirmed, feeCalc.est.pass.inMempool, feeCalc.est.pass.leftMempool,
              feeCalc.est.fail.start, feeCalc.est.fail.end,
              (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) > 0.0 ? 100 * feeCalc.est.fail.withinTarget / (feeCalc.est.fail.totalConfirmed + feeCalc.est.fail.inMempool + feeCalc.est.fail.leftMempool) : 0.0,
              feeCalc.est.fail.withinTarget, feeCalc.est.fail.totalConfirmed, feeCalc.est.fail.inMempool, feeCalc.est.fail.leftMempool);
    return true;
}

bool CreateTransaction(
        CWallet& wallet,
        const std::vector<CRecipient>& vecSend,
        CTransactionRef& tx,
        CAmount& nFeeRet,
        int& nChangePosInOut,
        bilingual_str& error,
        const CCoinControl& coin_control,
        FeeCalculation& fee_calc_out,
        bool sign,
        BlindDetails* blind_details,
        const IssuanceDetails* issuance_details)
{
    if (vecSend.empty()) {
        error = _("Transaction must have at least one recipient");
        return false;
    }

    if (std::any_of(vecSend.cbegin(), vecSend.cend(), [](const auto& recipient){ return recipient.nAmount < 0; })) {
        error = _("Transaction amounts must not be negative");
        return false;
    }

    // ELEMENTS
    if (g_con_elementsmode) {
        if (std::any_of(vecSend.cbegin(), vecSend.cend(), [](const auto& recipient){ return recipient.asset.IsNull(); })) {
            error = _("No asset provided for recipient");
            return false;
        }
    }

    LOCK(wallet.cs_wallet);

    int nChangePosIn = nChangePosInOut;
    Assert(!tx); // tx is an out-param. TODO change the return type from bool to tx (or nullptr)
    bool res = CreateTransactionInternal(wallet, vecSend, tx, nFeeRet, nChangePosInOut, error, coin_control, fee_calc_out, sign, blind_details, issuance_details);
    // try with avoidpartialspends unless it's enabled already
    if (res && nFeeRet > 0 /* 0 means non-functional fee rate estimation */ && wallet.m_max_aps_fee > -1 && !coin_control.m_avoid_partial_spends) {
        CCoinControl tmp_cc = coin_control;
        tmp_cc.m_avoid_partial_spends = true;
        CAmount nFeeRet2;
        CTransactionRef tx2;
        int nChangePosInOut2 = nChangePosIn;
        bilingual_str error2; // fired and forgotten; if an error occurs, we discard the results
        BlindDetails blind_details2;
        BlindDetails *blind_details2_ptr = blind_details ? &blind_details2 : nullptr;
        if (CreateTransactionInternal(wallet, vecSend, tx2, nFeeRet2, nChangePosInOut2, error2, tmp_cc, fee_calc_out, sign, blind_details2_ptr, issuance_details)) {
            // if fee of this alternative one is within the range of the max fee, we use this one
            const bool use_aps = nFeeRet2 <= nFeeRet + wallet.m_max_aps_fee;
            wallet.WalletLogPrintf("Fee non-grouped = %lld, grouped = %lld, using %s\n", nFeeRet, nFeeRet2, use_aps ? "grouped" : "non-grouped");
            if (use_aps) {
                tx = tx2;
                nFeeRet = nFeeRet2;
                nChangePosInOut = nChangePosInOut2;
                if (blind_details) {
                    *blind_details = blind_details2;
                }
            }
        }
    }
    return res;
}

bool FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& nFeeRet, int& nChangePosInOut, bilingual_str& error, bool lockUnspents, const std::set<int>& setSubtractFeeFromOutputs, CCoinControl coinControl)
{
    std::vector<CRecipient> vecSend;

    // Turn the txout set into a CRecipient vector.
    for (size_t idx = 0; idx < tx.vout.size(); idx++) {
        const CTxOut& txOut = tx.vout[idx];

        // ELEMENTS:
        if (!txOut.nValue.IsExplicit() || !txOut.nAsset.IsExplicit()) {
            error = _("Pre-funded amounts must be non-blinded");
            return false;
        }

        // Fee outputs should not be added to avoid overpayment of fees
        if (txOut.IsFee()) {
            continue;
        }

        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue.GetAmount(), txOut.nAsset.GetAsset(), CPubKey(txOut.nNonce.vchCommitment), setSubtractFeeFromOutputs.count(idx) == 1};
        vecSend.push_back(recipient);
    }

    coinControl.fAllowOtherInputs = true;

    for (const CTxIn& txin : tx.vin) {
        coinControl.Select(txin.prevout);
    }

    // Acquire the locks to prevent races to the new locked unspents between the
    // CreateTransaction call and LockCoin calls (when lockUnspents is true).
    LOCK(wallet.cs_wallet);

    CTransactionRef tx_new;
    FeeCalculation fee_calc_out;
    auto blind_details = g_con_elementsmode ? std::make_unique<BlindDetails>() : nullptr;
    if (!CreateTransaction(wallet, vecSend, tx_new, nFeeRet, nChangePosInOut, error, coinControl, fee_calc_out, false, blind_details.get())) {
        return false;
    }

    // Wipe outputs and output witness and re-add one by one
    tx.vout.clear();
    tx.witness.vtxoutwit.clear();
    for (unsigned int i = 0; i < tx_new->vout.size(); i++) {
        const CTxOut& out = tx_new->vout[i];
        tx.vout.push_back(out);
        if (tx_new->witness.vtxoutwit.size() > i) {
            // We want to re-add previously existing outwitnesses
            // even though we don't create any new ones
            const CTxOutWitness& outwit = tx_new->witness.vtxoutwit[i];
            tx.witness.vtxoutwit.push_back(outwit);
        }
    }

    // Add new txins while keeping original txin scriptSig/order.
    for (const CTxIn& txin : tx_new->vin) {
        if (!coinControl.IsSelected(txin.prevout)) {
            tx.vin.push_back(txin);

        }
        if (lockUnspents) {
            wallet.LockCoin(txin.prevout);
        }

    }

    return true;
}
} // namespace wallet
