/*
 * Copyright (c) 2020-2023 Revolution Populi Limited, and contributors.
 * Copyright (c) 2024 ActaBoards contributors.
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

#include <graphene/chain/room_evaluator.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/exceptions.hpp>

namespace graphene { namespace chain {

// ============ room_create_evaluator ============

void_result room_create_evaluator::do_evaluate( const room_create_operation& op )
{ try {
   database& d = db();

   // Check if room with this name already exists for this owner
   const auto& room_idx = d.get_index_type<room_index>();
   const auto& room_by_name = room_idx.indices().get<by_name>();
   auto itr = room_by_name.find(boost::make_tuple(op.owner, op.name));
   FC_ASSERT(itr == room_by_name.end(), "Room with this name already exists for this owner.");

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type room_create_evaluator::do_apply( const room_create_operation& o )
{ try {
   database& d = db();

   // Create the room
   const auto& new_room = d.create<room_object>( [&o]( room_object& obj )
   {
      obj.owner     = o.owner;
      obj.name      = o.name;
      obj.room_key  = o.room_key;
      obj.timestamp = time_point::now().sec_since_epoch();
   });

   // Automatically add owner as first participant
   d.create<room_participant_object>( [&o, &new_room]( room_participant_object& obj )
   {
      obj.room        = new_room.id;
      obj.participant = o.owner;
      obj.content_key = o.room_key;  // Owner has the same key
      obj.timestamp   = time_point::now().sec_since_epoch();
   });

   // Create epoch 0 key record for owner
   d.create<room_key_epoch_object>( [&o, &new_room]( room_key_epoch_object& obj )
   {
      obj.room        = new_room.id;
      obj.epoch       = 0;
      obj.participant = o.owner;
      obj.content_key = o.room_key;
   });

   return new_room.id;
} FC_CAPTURE_AND_RETHROW((o)) }

// ============ room_update_evaluator ============

void_result room_update_evaluator::do_evaluate( const room_update_operation& op )
{ try {
   database& d = db();

   _room = &d.get(op.room);
   FC_ASSERT(_room->owner == op.owner, "Only owner can update room.");

   // Check if new name conflicts with existing room
   if(_room->name != op.name) {
      const auto& room_idx = d.get_index_type<room_index>();
      const auto& room_by_name = room_idx.indices().get<by_name>();
      auto itr = room_by_name.find(boost::make_tuple(op.owner, op.name));
      FC_ASSERT(itr == room_by_name.end(), "Room with this name already exists.");
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type room_update_evaluator::do_apply( const room_update_operation& o )
{ try {
   database& d = db();

   d.modify(*_room, [&o](room_object& obj) {
      obj.name = o.name;
   });

   return _room->id;
} FC_CAPTURE_AND_RETHROW((o)) }

// ============ room_add_participant_evaluator ============

void_result room_add_participant_evaluator::do_evaluate( const room_add_participant_operation& op )
{ try {
   database& d = db();

   _room = &d.get(op.room);
   FC_ASSERT(_room->owner == op.owner, "Only owner can add participants.");

   // Check if participant is already in room
   const auto& participant_idx = d.get_index_type<room_participant_index>();
   const auto& by_room_part = participant_idx.indices().get<by_room_and_participant>();
   auto itr = by_room_part.find(boost::make_tuple(op.room, op.participant));
   FC_ASSERT(itr == by_room_part.end(), "Participant already in room.");

   // Validate epoch_keys: all must be historical (< current_epoch)
   for( const auto& ek : op.epoch_keys )
   {
      FC_ASSERT( ek.first < _room->current_epoch,
                 "Epoch key ${e} must reference a historical epoch (< current ${ce})",
                 ("e", ek.first)("ce", _room->current_epoch) );
      FC_ASSERT( !ek.second.empty(), "Epoch key value cannot be empty" );
   }

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type room_add_participant_evaluator::do_apply( const room_add_participant_operation& o )
{ try {
   database& d = db();

   const auto& new_participant = d.create<room_participant_object>( [&o]( room_participant_object& obj )
   {
      obj.room        = o.room;
      obj.participant = o.participant;
      obj.content_key = o.content_key;
      obj.timestamp   = time_point::now().sec_since_epoch();
   });

   // Create or update epoch key record for current epoch (handles re-add safely)
   const auto& epoch_idx = d.get_index_type<room_key_epoch_index>();
   const auto& by_rep = epoch_idx.indices().get<by_room_epoch_participant>();

   auto existing = by_rep.find(boost::make_tuple(o.room, _room->current_epoch, o.participant));
   if( existing == by_rep.end() )
   {
      d.create<room_key_epoch_object>( [&o, this]( room_key_epoch_object& obj )
      {
         obj.room        = o.room;
         obj.epoch       = _room->current_epoch;
         obj.participant = o.participant;
         obj.content_key = o.content_key;
      });
   }
   else
   {
      d.modify(*existing, [&o](room_key_epoch_object& obj) {
         obj.content_key = o.content_key;
      });
   }

   // Create or update historical epoch key records if provided
   for( const auto& epoch_entry : o.epoch_keys )
   {
      auto existing_hist = by_rep.find(boost::make_tuple(o.room, epoch_entry.first, o.participant));
      if( existing_hist == by_rep.end() )
      {
         d.create<room_key_epoch_object>( [&o, &epoch_entry]( room_key_epoch_object& obj )
         {
            obj.room        = o.room;
            obj.epoch       = epoch_entry.first;
            obj.participant = o.participant;
            obj.content_key = epoch_entry.second;
         });
      }
      else
      {
         d.modify(*existing_hist, [&epoch_entry](room_key_epoch_object& obj) {
            obj.content_key = epoch_entry.second;
         });
      }
   }

   return new_participant.id;
} FC_CAPTURE_AND_RETHROW((o)) }

// ============ room_remove_participant_evaluator ============

void_result room_remove_participant_evaluator::do_evaluate( const room_remove_participant_operation& op )
{ try {
   database& d = db();

   _participant = &d.get(op.participant_id);
   _room = &d.get(_participant->room);

   FC_ASSERT(_room->owner == op.owner, "Only owner can remove participants.");
   FC_ASSERT(_participant->participant != _room->owner, "Cannot remove owner from room.");

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type room_remove_participant_evaluator::do_apply( const room_remove_participant_operation& o )
{ try {
   database& d = db();

   object_id_type participant_id = _participant->id;
   d.remove(*_participant);

   return participant_id;
} FC_CAPTURE_AND_RETHROW((o)) }

// ============ room_rotate_key_evaluator ============

void_result room_rotate_key_evaluator::do_evaluate( const room_rotate_key_operation& op )
{ try {
   database& d = db();

   _room = &d.get(op.room);
   FC_ASSERT(_room->owner == op.owner, "Only owner can rotate room key.");

   // Collect current participants
   const auto& participant_idx = d.get_index_type<room_participant_index>();
   const auto& by_room_idx = participant_idx.indices().get<by_room>();

   flat_set<account_id_type> current_participants;
   auto itr = by_room_idx.lower_bound(op.room);
   while( itr != by_room_idx.end() && itr->room == op.room )
   {
      current_participants.insert(itr->participant);
      ++itr;
   }

   // Check that participant_keys covers all current participants
   for( const auto& p : current_participants )
   {
      FC_ASSERT( op.participant_keys.count(p) > 0,
                 "Missing key for participant ${p}", ("p", p) );
   }

   // Check that participant_keys doesn't contain non-participants
   for( const auto& pk : op.participant_keys )
   {
      FC_ASSERT( current_participants.count(pk.first) > 0,
                 "Key provided for non-participant ${p}", ("p", pk.first) );
   }

   // Owner's participant key must match new_room_key for consistency
   auto owner_key = op.participant_keys.find(op.owner);
   FC_ASSERT( owner_key != op.participant_keys.end(),
              "Owner must be included in participant_keys" );
   FC_ASSERT( owner_key->second == op.new_room_key,
              "Owner's participant key must match new_room_key" );

   return void_result();
} FC_CAPTURE_AND_RETHROW( (op) ) }

object_id_type room_rotate_key_evaluator::do_apply( const room_rotate_key_operation& o )
{ try {
   database& d = db();

   // Increment epoch and update room key
   uint32_t new_epoch = _room->current_epoch + 1;
   d.modify(*_room, [&o, new_epoch](room_object& obj) {
      obj.current_epoch = new_epoch;
      obj.room_key = o.new_room_key;
   });

   // Update participant content_keys and create epoch records
   const auto& participant_idx = d.get_index_type<room_participant_index>();
   const auto& by_room_idx = participant_idx.indices().get<by_room>();

   auto itr = by_room_idx.lower_bound(o.room);
   while( itr != by_room_idx.end() && itr->room == o.room )
   {
      auto pk_itr = o.participant_keys.find(itr->participant);
      if( pk_itr != o.participant_keys.end() )
      {
         d.modify(*itr, [&pk_itr](room_participant_object& obj) {
            obj.content_key = pk_itr->second;
         });

         // Create epoch key record
         d.create<room_key_epoch_object>( [&o, &pk_itr, new_epoch]( room_key_epoch_object& obj )
         {
            obj.room        = o.room;
            obj.epoch       = new_epoch;
            obj.participant = pk_itr->first;
            obj.content_key = pk_itr->second;
         });
      }
      ++itr;
   }

   return _room->id;
} FC_CAPTURE_AND_RETHROW((o)) }

} } // graphene::chain
