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

#include <graphene/protocol/room.hpp>
#include <fc/io/raw.hpp>

namespace graphene { namespace protocol {

// ============ room_create_operation ============

share_type room_create_operation::calculate_fee( const fee_parameters_type& k )const
{
   return k.fee;
}

void room_create_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( !name.empty(), "Room name cannot be empty" );
   FC_ASSERT( name.size() <= 256, "Room name too long (max 256 characters)" );
   FC_ASSERT( !room_key.empty(), "Room key cannot be empty" );
}

// ============ room_update_operation ============

share_type room_update_operation::calculate_fee( const fee_parameters_type& k )const
{
   return k.fee;
}

void room_update_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( !name.empty(), "Room name cannot be empty" );
   FC_ASSERT( name.size() <= 256, "Room name too long (max 256 characters)" );
}

// ============ room_add_participant_operation ============

share_type room_add_participant_operation::calculate_fee( const fee_parameters_type& k )const
{
   return k.fee;
}

void room_add_participant_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( !content_key.empty(), "Content key cannot be empty" );
}

// ============ room_remove_participant_operation ============

share_type room_remove_participant_operation::calculate_fee( const fee_parameters_type& k )const
{
   return k.fee;
}

void room_remove_participant_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
}

// ============ room_rotate_key_operation ============

share_type room_rotate_key_operation::calculate_fee( const fee_parameters_type& k )const
{
   share_type core_fee_required = k.fee;
   // Calculate size of participant_keys
   size_t total_size = new_room_key.size();
   for( const auto& p : participant_keys )
      total_size += p.second.size();
   core_fee_required += share_type( (total_size * k.price_per_kbyte) / 1024 );
   return core_fee_required;
}

void room_rotate_key_operation::validate()const
{
   FC_ASSERT( fee.amount >= 0 );
   FC_ASSERT( !new_room_key.empty(), "New room key cannot be empty" );
   FC_ASSERT( !participant_keys.empty(), "Participant keys cannot be empty (at least owner required)" );
}

} } // graphene::protocol

GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_create_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_create_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_update_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_update_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_add_participant_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_add_participant_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_remove_participant_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_remove_participant_operation )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_rotate_key_operation::fee_parameters_type )
GRAPHENE_IMPLEMENT_EXTERNAL_SERIALIZATION( graphene::protocol::room_rotate_key_operation )
