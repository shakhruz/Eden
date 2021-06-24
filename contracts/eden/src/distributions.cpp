#include <accounts.hpp>
#include <distributions.hpp>
#include <elections.hpp>
#include <members.hpp>

namespace eden
{
   void init_pools(eosio::name contract)
   {
      pool_table_type pool_tb{contract, default_scope};
      pool_tb.emplace(contract, [](auto& row) { row.value = pool_v0{"master"_n, 5}; });
   }

   static current_distribution make_distribution(eosio::name contract,
                                                 eosio::block_timestamp start_time,
                                                 eosio::asset amount)
   {
      members members{contract};
      current_distribution result{start_time, eosio::name()};
      auto ranks = members.stats().ranks;
      auto per_rank = amount / (ranks.size() - 1);
      uint16_t total = 0;
      eosio::asset remaining = amount;
      for (auto iter = ranks.end() - 1, end = ranks.begin(); iter != end; --iter)
      {
         total += *iter;
         auto this_rank = per_rank / total;
         remaining -= this_rank * total;
         result.rank_distribution.push_back(this_rank);
      }
      std::reverse(result.rank_distribution.begin(), result.rank_distribution.end());
      result.rank_distribution.back() += remaining;
      return result;
   }

   void process_election_distribution(eosio::name contract)
   {
      distribution_table_type distribution_tb{contract, default_scope};
      for (auto iter = distribution_tb.begin(), end = distribution_tb.end(); iter != end; ++iter)
      {
         if (auto* dist = std::get_if<election_distribution>(&iter->value))
         {
            distribution_tb.modify(iter, contract, [&](auto& row) {
               row.value = make_distribution(contract, dist->distribution_time, dist->amount);
            });
         }
         else if (std::holds_alternative<next_distribution>(iter->value))
         {
            break;
         }
      }
   }

   bool setup_distribution(eosio::name contract, eosio::block_timestamp init)
   {
      distribution_table_type distribution_tb{contract, default_scope};
      auto iter = distribution_tb.end();
      if (iter == distribution_tb.begin())
      {
         if (init != eosio::block_timestamp())
         {
            distribution_tb.emplace(contract,
                                    [&](auto& row) { row.value = next_distribution{init}; });
         }
         else
         {
            return false;
         }
      }
      --iter;
      bool result = false;
      auto next_election_time = elections{contract}.get_next_election_time();
      while (true)
      {
         eosio::block_timestamp distribution_time;
         if (auto* next = std::get_if<next_distribution>(&iter->value))
         {
            if (next->distribution_time <= eosio::current_block_time())
            {
               distribution_time = next->distribution_time;
            }
            else
            {
               return result;
            }
         }
         else
         {
            eosio::check(false, "Invariant failure: no next distribution");
         }
         eosio::block_timestamp next_time{distribution_time.to_time_point() + eosio::days(30)};
         std::optional<uint128_t> prorate_num;
         constexpr uint32_t prorate_den = 30 * 24 * 60 * 60 * 100;
         if (next_election_time)
         {
            if (*next_election_time<next_time&& * next_election_time> distribution_time)
            {
               next_time = *next_election_time;
               prorate_num = (next_time.slot - distribution_time.slot) / 2;
            }
         }
         pool_table_type pool_tb{contract, default_scope};
         if (pool_tb.begin() == pool_tb.end())
         {
            pool_tb.emplace(contract, [](auto& row) { row.value = pool_v0{"master"_n, 5}; });
         }
         accounts accounts{contract, "owned"_n};
         class accounts dist_account
         {
            contract, make_account_scope(distribution_time, 0)
         };
         for (const auto& pool : pool_tb)
         {
            auto account = accounts.get_account(pool.name());
            if (account)
            {
               auto amount = prorate_num
                                 ? eosio::asset(*prorate_num * pool.monthly_distribution_pct() *
                                                    account->balance().amount / prorate_den,
                                                account->balance().symbol)
                                 : (pool.monthly_distribution_pct() * account->balance() / 100);
               accounts.sub_balance(pool.name(), amount);
               dist_account.add_balance(contract, amount);
            }
         }
         auto total = dist_account.get_account(contract);
         if (total)
         {
            distribution_tb.modify(iter, contract, [&](auto& row) {
               if (next_election_time && *next_election_time > distribution_time)
               {
                  row.value = make_distribution(contract, distribution_time, total->balance());
               }
               else
               {
                  row.value = election_distribution{distribution_time, total->balance()};
               }
            });
         }
         else
         {
            distribution_tb.erase(iter);
         }
         iter = distribution_tb.emplace(
             contract, [&](auto& row) { row.value = next_distribution{next_time}; });
         result = true;
      }
   }

   uint32_t distribute_monthly(eosio::name contract, uint32_t max_steps, current_distribution& dist)
   {
      members members{contract};
      std::vector<accounts> accounts_by_rank;
      accounts accounts{contract, make_account_scope(dist.distribution_time, 0)};
      accounts_by_rank.reserve(dist.rank_distribution.size());
      auto& table = members.get_table();
      auto iter = table.upper_bound(dist.last_processed.value);
      auto end = table.end();
      for (; max_steps > 0 && iter != end; ++iter, --max_steps)
      {
         auto member = *iter;
         eosio::check(iter->election_rank() <= dist.rank_distribution.size(),
                      "Invariant failure: rank too high");
         while (accounts_by_rank.size() < iter->election_rank())
         {
            accounts_by_rank.emplace_back(
                contract, make_account_scope(dist.distribution_time, accounts_by_rank.size() + 1));
         }
         for (uint8_t rank = 0; rank < iter->election_rank(); ++rank)
         {
            auto amount = dist.rank_distribution[rank];
            accounts_by_rank[rank].add_balance(iter->account(), amount);
            accounts.sub_balance(contract, amount);
         }
         dist.last_processed = iter->account();
      }
      return max_steps;
   }

   uint32_t distribute_monthly(eosio::name contract, uint32_t max_steps)
   {
      if (max_steps > 0)
      {
         if (setup_distribution(contract))
         {
            --max_steps;
         }
      }
      distribution_table_type distribution_tb{contract, default_scope};
      for (auto iter = distribution_tb.begin(), end = distribution_tb.end();
           max_steps && iter != end;)
      {
         if (auto* current = std::get_if<current_distribution>(&iter->value))
         {
            auto copy = *current;
            max_steps = distribute_monthly(contract, max_steps, copy);
            if (max_steps)
            {
               iter = distribution_tb.erase(iter);
               --max_steps;
            }
            else
            {
               distribution_tb.modify(iter, contract, [&](auto& row) { row.value = copy; });
               return max_steps;
            }
         }
         else if (std::holds_alternative<next_distribution>(iter->value))
         {
            return max_steps;
         }
         else if (auto* election = std::get_if<election_distribution>(&iter->value))
         {
            // We're still in the election.  election_distribution should
            // be converted to current_distribution when the election is
            // finished.
            return max_steps;
         }
         else
         {
            eosio::check(false, "Invariant failure: unexpected distribution type");
         }
      }
      return max_steps;
   }

   uint32_t distributions::gc(uint32_t max_steps)
   {
      distribution_point_table_type distribution_point_tb{contract, default_scope};
      for (auto iter = distribution_point_tb.begin(), end = distribution_point_tb.end();
           max_steps > 0 && iter != end; --max_steps)
      {
         account_table_type account_tb{contract, iter->primary_key()};
         if (account_tb.begin() != account_tb.end())
         {
            break;
         }
         iter = distribution_point_tb.erase(iter);
      }
      return max_steps;
   }

   void distributions::clear_all()
   {
      distribution_point_table_type distribution_point_tb{contract, default_scope};
      for (auto iter = distribution_point_tb.begin(), end = distribution_point_tb.end();
           iter != end;)
      {
         accounts accounts(contract, eosio::name(iter->primary_key()));
         accounts.clear_all();
         iter = distribution_point_tb.erase(iter);
      }
      pool_table_type pool_tb{contract, default_scope};
      for (auto iter = pool_tb.begin(), end = pool_tb.end(); iter != end;)
      {
         iter = pool_tb.erase(iter);
      }
      distribution_table_type distribution_tb{contract, default_scope};
      for (auto iter = distribution_tb.begin(), end = distribution_tb.end(); iter != end;)
      {
         iter = distribution_tb.erase(iter);
      }
   }
}  // namespace eden
