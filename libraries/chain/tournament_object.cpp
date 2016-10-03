/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <graphene/chain/database.hpp>
#include <graphene/chain/tournament_object.hpp>
#include <graphene/chain/match_object.hpp>

#include <boost/msm/back/state_machine.hpp>
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/msm/back/tools.hpp>

#include <fc/crypto/hash_ctr_rng.hpp>

namespace graphene { namespace chain {

   namespace msm = boost::msm;
   namespace mpl = boost::mpl;

   namespace 
   {
      // Events
      struct player_registered
      {
         database& db;
         account_id_type payer_id;
         account_id_type player_id;
         player_registered(database& db, account_id_type payer_id, account_id_type player_id) : 
            db(db), payer_id(payer_id), player_id(player_id) 
         {}
      };
      struct registration_deadline_passed 
      {
         database& db;
         registration_deadline_passed(database& db) : db(db) {};
      };
      struct start_time_arrived 
      {
         database& db;
         start_time_arrived(database& db) : db(db) {};
      };
      struct final_game_completed {};

      struct tournament_state_machine_ : public msm::front::state_machine_def<tournament_state_machine_>
      {
         // disable a few state machine features we don't use for performance
         typedef int no_exception_thrown;
         typedef int no_message_queue;

         // States
         struct accepting_registrations : public msm::front::state<>{};
         struct awaiting_start : public msm::front::state<>
         {
            void on_entry(const player_registered& event, tournament_state_machine_& fsm)
            {
               fc_ilog(fc::logger::get("tournament"),
                       "Tournament ${id} now has enough players registered to begin",
                       ("id", fsm.tournament_obj->id));
               if (fsm.tournament_obj->options.start_time)
                  fsm.tournament_obj->start_time = fsm.tournament_obj->options.start_time;
               else
                  fsm.tournament_obj->start_time = event.db.head_block_time() + fc::seconds(*fsm.tournament_obj->options.start_delay);
            }
         };
         struct in_progress : public msm::front::state<>
         {
            // reverse the bits in an integer
            static uint32_t reverse_bits(uint32_t x)
            {
               x = (((x & 0xaaaaaaaa) >> 1) | ((x & 0x55555555) << 1));
               x = (((x & 0xcccccccc) >> 2) | ((x & 0x33333333) << 2));
               x = (((x & 0xf0f0f0f0) >> 4) | ((x & 0x0f0f0f0f) << 4));
               x = (((x & 0xff00ff00) >> 8) | ((x & 0x00ff00ff) << 8));
               return ((x >> 16) | (x << 16));
            }

            match_id_type create_match(database& db, tournament_id_type tournament_id, 
                                       const vector<account_id_type>& players)
            {
               const match_object& match =
                  db.create<match_object>( [&]( match_object& match ) {
                     match.tournament_id = tournament_id;
                     match.players = players;
                     match.start_time = db.head_block_time();
                     if (match.players.size() == 1)
                     {
                        // this is a bye
                        match.end_time = db.head_block_time();
                     }
                  });
               return match.id;
            }
            
            void on_entry(const start_time_arrived& event, tournament_state_machine_& fsm)
            {
               fc_ilog(fc::logger::get("tournament"),
                       "Tournament ${id} is beginning",
                       ("id", fsm.tournament_obj->id));
               const tournament_details_object& tournament_details_obj = fsm.tournament_obj->tournament_details_id(event.db);

               // TODO hoist the rng to reset once per block?
               fc::hash_ctr_rng<secret_hash_type, 20> rng(event.db.get_dynamic_global_properties().random.data());

               // Create the "seeding" order for the tournament as a random shuffle of the players.
               //
               // If this were a game of skill where players were ranked, this algorithm expects the 
               // most skilled players to the front of the list
               vector<account_id_type> seeded_players(tournament_details_obj.registered_players.begin(), 
                                                      tournament_details_obj.registered_players.end());
               for (unsigned i = seeded_players.size() - 1; i >= 1; --i)
               {
                  unsigned j = (unsigned)rng(i + 1);
                  std::swap(seeded_players[i], seeded_players[j]);
               }

               // Create all matches in the tournament now.
               // If the number of players isn't a power of two, we will compensate with  byes 
               // in the first round.  
               const uint32_t num_players = fsm.tournament_obj->options.number_of_players;
               uint32_t num_rounds = boost::multiprecision::detail::find_msb(num_players - 1) + 1;
               uint32_t num_matches = (1 << num_rounds) - 1;
               uint32_t num_matches_in_first_round = 1 << (num_rounds - 1);

               // First, assign players to their first round of matches in the paired_players
               // array, where the first two play against each other, the second two play against
               // each other, etc.
               // Anyone with account_id_type() as their opponent gets a bye
               vector<account_id_type> paired_players;
               paired_players.resize(num_matches_in_first_round * 2);
               for (uint32_t player_num = 0; player_num < num_players; ++player_num)
               {
                  uint32_t player_position = reverse_bits(player_num ^ (player_num >> 1)) >> (32 - num_rounds);
                  paired_players[player_position] = seeded_players[player_num];
               }

               // now create the match objects for this first round
               vector<match_id_type> matches;
               matches.reserve(num_matches);

               // create a bunch of empty matches
               for (unsigned i = 0; i < num_matches; ++i)
                  matches.emplace_back(create_match(event.db, fsm.tournament_obj->id, vector<account_id_type>()));

               // then walk through our paired players by twos, starting the first matches
               for (unsigned i = 0; i < num_matches_in_first_round; ++i)
               {
                  vector<account_id_type> players;
                  players.emplace_back(paired_players[2 * i]);
                  if (paired_players[2 * i + 1] != account_id_type())
                     players.emplace_back(paired_players[2 * i + 1]);
                  event.db.modify(matches[i](event.db), [&](match_object& match) {
                     match.on_initiate_match(event.db, players);
                     });
               }
               event.db.modify(tournament_details_obj, [&](tournament_details_object& tournament_details_obj){
                  tournament_details_obj.matches = matches;
               });
            }
         };
         struct registration_period_expired : public msm::front::state<>
         {
            void on_entry(const registration_deadline_passed& event, tournament_state_machine_& fsm)
            {
               fc_ilog(fc::logger::get("tournament"),
                       "Tournament ${id} is canceled",
                       ("id", fsm.tournament_obj->id));
               // repay everyone who paid into the prize pool
               const tournament_details_object& details = fsm.tournament_obj->tournament_details_id(event.db);
               for (const auto& payer_pair : details.payers)
               {
                  // TODO: create a virtual operation to record the refund
                  // we'll think of this as just releasing an asset that the user had locked up
                  // for a period of time, not as a transfer back to the user; it doesn't matter
                  // if they are currently authorized to transfer this asset, they never really 
                  // transferred it in the first place
                  event.db.adjust_balance(payer_pair.first, asset(payer_pair.second, fsm.tournament_obj->options.buy_in.asset_id));
               }
            }
         };
         struct concluded : public msm::front::state<>{};

         typedef accepting_registrations initial_state;

         typedef tournament_state_machine_ x; // makes transition table cleaner
         
         // Guards
         bool will_be_fully_registered(const player_registered& event)
         {
            fc_ilog(fc::logger::get("tournament"),
                    "In will_be_fully_registered guard, returning ${value}",
                    ("value", tournament_obj->registered_players == tournament_obj->options.number_of_players - 1));
            return tournament_obj->registered_players == tournament_obj->options.number_of_players - 1;
         }
         
         void register_player(const player_registered& event)
         {
            fc_ilog(fc::logger::get("tournament"),
                    "In register_player action, player_id is ${player_id}, payer_id is ${payer_id}",
                    ("player_id", event.player_id)("payer_id", event.payer_id));

            event.db.adjust_balance(event.payer_id, -tournament_obj->options.buy_in);
            const tournament_details_object& tournament_details_obj = tournament_obj->tournament_details_id(event.db);
            event.db.modify(tournament_details_obj, [&](tournament_details_object& tournament_details_obj){
                    tournament_details_obj.payers[event.payer_id] += tournament_obj->options.buy_in.amount;
                    tournament_details_obj.registered_players.insert(event.player_id);
                 });
            ++tournament_obj->registered_players;
            tournament_obj->prize_pool += tournament_obj->options.buy_in.amount;
         }

         // Transition table for tournament
         struct transition_table : mpl::vector<
         //    Start                       Event                         Next                       Action               Guard
         //  +---------------------------+-----------------------------+----------------------------+---------------------+----------------------+
         a_row < accepting_registrations, player_registered,            accepting_registrations,     &x::register_player >,
         row   < accepting_registrations, player_registered,            awaiting_start,              &x::register_player, &x::will_be_fully_registered >,
         _row  < accepting_registrations, registration_deadline_passed, registration_period_expired >,
         //  +---------------------------+-----------------------------+----------------------------+---------------------+----------------------+
         _row  < awaiting_start,          start_time_arrived,           in_progress >,
         //  +---------------------------+-----------------------------+----------------------------+---------------------+----------------------+
         _row  < in_progress,             final_game_completed,         concluded >
         //  +---------------------------+-----------------------------+----------------------------+---------------------+----------------------+
         > {};


         tournament_object* tournament_obj;
         tournament_state_machine_(tournament_object* tournament_obj) : tournament_obj(tournament_obj) {}
      };
      typedef msm::back::state_machine<tournament_state_machine_> tournament_state_machine;
   }

   class tournament_object::impl {
   public:
      tournament_state_machine state_machine;

      impl(tournament_object* self) : state_machine(self) {}
   };

   tournament_object::tournament_object() :
      my(new impl(this))
   {
   }

   tournament_object::tournament_object(const tournament_object& rhs) : 
      graphene::db::abstract_object<tournament_object>(rhs),
      creator(rhs.creator),
      options(rhs.options),
      start_time(rhs.start_time),
      end_time(rhs.end_time),
      prize_pool(rhs.prize_pool),
      registered_players(rhs.registered_players),
      tournament_details_id(rhs.tournament_details_id),
      my(new impl(this))
   {
      my->state_machine = rhs.my->state_machine;
      my->state_machine.tournament_obj = this;
   }

   tournament_object& tournament_object::operator=(const tournament_object& rhs)
   {
      //graphene::db::abstract_object<tournament_object>::operator=(rhs);
      id = rhs.id;
      creator = rhs.creator;
      options = rhs.options;
      start_time = rhs.start_time;
      end_time = rhs.end_time;
      prize_pool = rhs.prize_pool;
      registered_players = rhs.registered_players;
      tournament_details_id = rhs.tournament_details_id;
      my->state_machine = rhs.my->state_machine;
      my->state_machine.tournament_obj = this;

      return *this;
   }

   tournament_object::~tournament_object()
   {
   }

   bool verify_tournament_state_constants()
   {
      unsigned error_count = 0;
      typedef msm::back::generate_state_set<tournament_state_machine::stt>::type all_states;
      static char const* filled_state_names[mpl::size<all_states>::value];
      mpl::for_each<all_states,boost::msm::wrap<mpl::placeholders::_1> >
         (msm::back::fill_state_names<tournament_state_machine::stt>(filled_state_names));
      for (unsigned i = 0; i < mpl::size<all_states>::value; ++i)
      {
         try
         {
            // this is an approximate test, the state name provided by typeinfo will be mangled, but should
            // at least contain the string we're looking for
            const char* fc_reflected_value_name = fc::reflector<tournament_state>::to_string((tournament_state)i);
            if (!strcmp(fc_reflected_value_name, filled_state_names[i]))
               fc_elog(fc::logger::get("tournament"),
                       "Error, state string mismatch between fc and boost::msm for int value ${int_value}: boost::msm -> ${boost_string}, fc::reflect -> ${fc_string}",
                       ("int_value", i)("boost_string", filled_state_names[i])("fc_string", fc_reflected_value_name));
         }
         catch (const fc::bad_cast_exception&)
         {
            fc_elog(fc::logger::get("tournament"),
                    "Error, no reflection for value ${int_value} in enum tournament_state",
                    ("int_value", i));
            ++error_count;
         }
      }

      return error_count == 0;
   }

   tournament_state tournament_object::get_state() const
   {
      static bool state_constants_are_correct = verify_tournament_state_constants();
      (void)&state_constants_are_correct;
      tournament_state state = (tournament_state)my->state_machine.current_state()[0];

      return state;  
   }

   void tournament_object::pack_impl(std::ostream& stream) const
   {
      boost::archive::binary_oarchive oa(stream, boost::archive::no_header|boost::archive::no_codecvt|boost::archive::no_xml_tag_checking);
      oa << my->state_machine;
   }

   void tournament_object::unpack_impl(std::istream& stream)
   {
      boost::archive::binary_iarchive ia(stream, boost::archive::no_header|boost::archive::no_codecvt|boost::archive::no_xml_tag_checking);
      ia >> my->state_machine;
   }

   void tournament_object::on_registration_deadline_passed(database& db)
   {
      my->state_machine.process_event(registration_deadline_passed(db));
   }

   void tournament_object::on_player_registered(database& db, account_id_type payer_id, account_id_type player_id)
   {
      my->state_machine.process_event(player_registered(db, payer_id, player_id));
   }

   void tournament_object::on_start_time_arrived(database& db)
   {
      my->state_machine.process_event(start_time_arrived(db));
   }

   void tournament_object::on_final_game_completed()
   {
      my->state_machine.process_event(final_game_completed());
   }

   void tournament_object::check_for_new_matches_to_start(database& db) const
   {
      const tournament_details_object& tournament_details_obj = tournament_details_id(db);

      unsigned num_matches = tournament_details_obj.matches.size();
      uint32_t num_rounds = boost::multiprecision::detail::find_msb(num_matches + 1);

      // Scan the matches by round to find the last round where all matches are complete
      int last_complete_round = -1;
      bool first_incomplete_match_was_waiting = false;
      for (unsigned round_num = 0; round_num < num_rounds; ++round_num)
      {
         uint32_t num_matches_in_this_round = 1 << (num_rounds - round_num - 1);
         uint32_t first_match_in_round = (num_matches - (num_matches >> round_num));
         bool all_matches_in_round_complete = true;
         for (uint32_t match_num = first_match_in_round; match_num < first_match_in_round + num_matches_in_this_round; ++match_num)
         {
            const match_object& match = tournament_details_obj.matches[match_num](db);
            if (match.get_state() != match_state::match_complete)
            {
               first_incomplete_match_was_waiting = match.get_state() == match_state::waiting_on_previous_matches;
               all_matches_in_round_complete = false;
               break;
            }
         }
         if (all_matches_in_round_complete)
            last_complete_round = round_num;
         else
            break;
      }

      if (last_complete_round == -1)
         return;

      // We shouldn't be here if the final match is complete
      assert(last_complete_round != num_rounds - 1);
      if (last_complete_round == num_rounds - 1)
         return;

      if (first_incomplete_match_was_waiting)
      {
         // all previous matches have completed, and the first match in this round hasn't been
         // started (which means none of the matches in this round should have started)
         unsigned first_incomplete_round = last_complete_round + 1;
         uint32_t num_matches_in_incomplete_round = 1 << (num_rounds - first_incomplete_round - 1);
         uint32_t first_match_in_incomplete_round = num_matches - (num_matches >> first_incomplete_round);
         for (uint32_t match_num = first_match_in_incomplete_round; 
              match_num < first_match_in_incomplete_round + num_matches_in_incomplete_round;
              ++match_num)
         {
            int left_child_index = (num_matches - 1) - ((num_matches - 1 - match_num) * 2 + 2);
            int right_child_index = left_child_index + 1;
            const match_object& match_to_start = tournament_details_obj.matches[left_child_index](db);
            const match_object& left_match = tournament_details_obj.matches[left_child_index](db);
            const match_object& right_match = tournament_details_obj.matches[right_child_index](db);
            std::vector<account_id_type> winners;
            if (!left_match.match_winners.empty())
            {
               assert(left_match.match_winners.size() == 1);
               winners.emplace_back(*left_match.match_winners.begin());
            }
            if (!right_match.match_winners.empty())
            {
               assert(right_match.match_winners.size() == 1);
               winners.emplace_back(*right_match.match_winners.begin());
            }
            db.modify(match_to_start, [&](match_object& match) {
               match.players = winners;
               //match.state = ready_to_begin;
            });

         }
      }
   }

} } // graphene::chain

namespace fc { 
   // Manually reflect tournament_object to variant to properly reflect "state"
   void to_variant(const graphene::chain::tournament_object& tournament_obj, fc::variant& v)
   {
      fc_elog(fc::logger::get("tournament"), "In tournament_obj to_variant");
      elog("In tournament_obj to_variant");
      fc::mutable_variant_object o;
      o("id", tournament_obj.id)
       ("creator", tournament_obj.creator)
       ("options", tournament_obj.options)
       ("start_time", tournament_obj.start_time)
       ("end_time", tournament_obj.end_time)
       ("prize_pool", tournament_obj.prize_pool)
       ("registered_players", tournament_obj.registered_players)
       ("tournament_details_id", tournament_obj.tournament_details_id)
       ("state", tournament_obj.get_state());

      v = o;
   }

   // Manually reflect tournament_object to variant to properly reflect "state"
   void from_variant(const fc::variant& v, graphene::chain::tournament_object& tournament_obj)
   {
      fc_elog(fc::logger::get("tournament"), "In tournament_obj from_variant");
      tournament_obj.id = v["id"].as<graphene::chain::tournament_id_type>();
      tournament_obj.creator = v["creator"].as<graphene::chain::account_id_type>();
      tournament_obj.options = v["options"].as<graphene::chain::tournament_options>();
      tournament_obj.start_time = v["start_time"].as<optional<time_point_sec> >();
      tournament_obj.end_time = v["end_time"].as<optional<time_point_sec> >();
      tournament_obj.prize_pool = v["prize_pool"].as<graphene::chain::share_type>();
      tournament_obj.registered_players = v["registered_players"].as<uint32_t>();
      tournament_obj.tournament_details_id = v["tournament_details_id"].as<graphene::chain::tournament_details_id_type>();
      graphene::chain::tournament_state state = v["state"].as<graphene::chain::tournament_state>();
      const_cast<int*>(tournament_obj.my->state_machine.current_state())[0] = (int)state;
   }
} //end namespace fc


