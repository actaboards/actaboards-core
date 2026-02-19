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

#pragma once

#include <graphene/chain/evaluator.hpp>
#include <graphene/chain/room_object.hpp>

namespace graphene { namespace chain {

class room_create_evaluator : public evaluator<room_create_evaluator>
{
public:
   typedef room_create_operation operation_type;

   void_result do_evaluate( const room_create_operation& o );
   object_id_type do_apply( const room_create_operation& o );
};

class room_update_evaluator : public evaluator<room_update_evaluator>
{
public:
   typedef room_update_operation operation_type;

   void_result do_evaluate( const room_update_operation& o );
   object_id_type do_apply( const room_update_operation& o );

private:
   const room_object* _room = nullptr;
};

class room_add_participant_evaluator : public evaluator<room_add_participant_evaluator>
{
public:
   typedef room_add_participant_operation operation_type;

   void_result do_evaluate( const room_add_participant_operation& o );
   object_id_type do_apply( const room_add_participant_operation& o );

private:
   const room_object* _room = nullptr;
};

class room_remove_participant_evaluator : public evaluator<room_remove_participant_evaluator>
{
public:
   typedef room_remove_participant_operation operation_type;

   void_result do_evaluate( const room_remove_participant_operation& o );
   object_id_type do_apply( const room_remove_participant_operation& o );

private:
   const room_participant_object* _participant = nullptr;
   const room_object* _room = nullptr;
};

class room_rotate_key_evaluator : public evaluator<room_rotate_key_evaluator>
{
public:
   typedef room_rotate_key_operation operation_type;

   void_result do_evaluate( const room_rotate_key_operation& o );
   object_id_type do_apply( const room_rotate_key_operation& o );

private:
   const room_object* _room = nullptr;
};

} } // graphene::chain
