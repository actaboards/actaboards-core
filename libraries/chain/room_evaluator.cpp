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

} } // graphene::chain
