#include <stdlib.h>
#include <string.h>
#include <util.h>
#include <subst.h>
#include <vector.h>
#include <node_data.h>
#include <buffer.h>
#include <subst_func.h>
#include <parser.h>

/**
   This file implements a small support struct for search-replace
   operations, along with wrapped calls to util_string_replace_inplace().
   
   Substitutions can be carried out on files and string in memory (char *
   with \0 termination); and the operations can be carried out inplace, or
   in a filtering mode where a new file/string is created.


   Usage
   =====
    1. Start with allocating a subst_list instance with subst_list_alloc().

    2. Insert key,value pairs to for search-replace with the functions

       	* subst_list_insert_ref(subst_list , key , value);
       	* subst_list_insert_owned_ref(subst_list , key , value);
       	* subst_list_insert_copy(subst_list , key , value );  
   
       The difference between these functions is who is owning the memory
       pointed to by the value pointer.

    3. Do the actual search-replace operation on a file or memory buffer:
    
       * subst_list_filter_file()   : Does search-replace on a file.
       * subst_list_update_string() : Does search-replace on a buffer.
       
    4. Free the subst_list and go home.

   Internally the (key,value) pairs used for substitutions are stored in a
   vector, preserving insert order. If you insert the cascade

     ("A","B")
     ("B","C")
       .....
     ("Y","Z")

   You will eventually end up with a string where all capital letters have
   been transformed to 'Z'.
*/


typedef enum { SUBST_DEEP_COPY   = 1,     
	       SUBST_MANAGED_REF = 2,
	       SUBST_SHARED_REF  = 3} subst_insert_type; /* Mode used in the subst_list_insert__() function */
  

struct subst_list_struct {
  vector_type           * string_data;  /* The string substitutions we should do. */
  vector_type           * func_data;    /* The functions we support. */
  subst_func_pool_type  * func_pool;    /* NOT owned by the subst_list instance - can be NULL */
};



/* 
   The subst_list type is implemented as a hash of subst_list_node
   instances. This node type is not exported out of this file scope at
   all.
*/
   

typedef struct {  
  bool                   value_owner;     /* Wether the memory pointed to by value should bee freed.*/
  char                 * value;
  char                 * key;      
  /*-- Callback code --*/
  subst_callback_ftype * callback;
  void                 * callback_arg;
  free_ftype           * free_callback_arg;
} subst_list_string_type;



/** Allocates an empty instance with no values. */
static subst_list_string_type * subst_list_node_alloc(const char * key) {
  subst_list_string_type * node = util_malloc(sizeof * node , __func__);
  node->value             = NULL;
  node->value_owner       = false;
  node->key               = util_alloc_string_copy( key );
  node->callback          = NULL;
  node->callback_arg      = NULL;
  node->free_callback_arg = NULL;
  return node;
}


static void subst_list_node_free_content( subst_list_string_type * node ) {
  if (node->value_owner)
    util_safe_free( node->value );

  if (node->free_callback_arg != NULL)
    node->free_callback_arg( node->callback_arg );
}


static void subst_list_node_free(subst_list_string_type * node) {
  subst_list_node_free_content( node );
  free(node->key);
  free(node);
}


static void subst_list_node_free__(void * node) {
  subst_list_node_free( (subst_list_string_type *) node );
}




static void subst_list_node_set_callback(subst_list_string_type * node, subst_callback_ftype * callback , void * callback_arg , free_ftype * free_callback_arg) {
  subst_list_node_free_content( node );
  {
    node->value             = NULL;
    node->callback          = callback;
    node->callback_arg      = callback_arg;
    node->free_callback_arg = free_callback_arg;
  }
}


/**
   __value can be NULL. 
*/
static void subst_list_node_set_value(subst_list_string_type * node, const char * __value , subst_insert_type insert_mode) {
  subst_list_node_free_content( node );
  {
    char * value;
    if (insert_mode == SUBST_DEEP_COPY)
      value = util_alloc_string_copy(__value);
    else
      value = (char *) __value;
    
    if (insert_mode == SUBST_SHARED_REF)
      node->value_owner = false;
    else
      node->value_owner = true;
    
    node->value = value;
  }
}


/**
   Find the node corresponding to 'key' -  returning NULL if it is not found.
*/

static subst_list_string_type * subst_list_get_node(const subst_list_type * subst_list , const char * key) {
  subst_list_string_type * node = NULL;
  int  index                  = 0;

  /* Linear search ... */
  while ((index < vector_get_size(subst_list->string_data)) && (node == NULL)) {
    subst_list_string_type * inode = vector_iget( subst_list->string_data , index);

    if (strcmp(inode->key , key) == 0)  /* Found it */
      node = inode;
    else
      index++;
  }

  return node;
}



static subst_list_string_type * subst_list_insert_new_node(subst_list_type * subst_list , const char * key) {
  subst_list_string_type * new_node = subst_list_node_alloc(key);
  vector_append_owned_ref(subst_list->string_data , new_node , subst_list_node_free__);
  return new_node;
}


subst_list_type * subst_list_alloc(subst_func_pool_type * func_pool) {
  subst_list_type * subst_list = util_malloc(sizeof * subst_list , __func__);
  subst_list->string_data      = vector_alloc_new();
  subst_list->func_data        = vector_alloc_new();
  subst_list->func_pool = func_pool;
  return subst_list;
}


static void subst_list_insert__(subst_list_type * subst_list , const char * key , const char * value , subst_insert_type insert_mode) {
  subst_list_string_type * node = subst_list_get_node(subst_list , key);
  
  if (node == NULL) /* Did not have the node. */
    node = subst_list_insert_new_node(subst_list , key);
  
  subst_list_node_set_value(node , value , insert_mode);
}


/**
   There are three different functions for inserting a key-value pair
   in the subst_list instance. The difference between the three is in
   which scope takes/has ownership of 'value'. The alternatives are:

    subst_list_insert_ref: In this case the calling scope has full
       ownership of value, and is consquently responsible for freeing
       it, and ensuring that it stays a valid pointer for the subst_list
       instance. Probably the most natural function to use when used
       with static storage, i.e. typically string literals.

    subst_list_insert_owned_ref: In this case the subst_list takes
       ownership of the value reference, in the sense that it will
       free it when it is done.

    subst_list_insert_copy: In this case the subst_list takes a copy
       of value and inserts it. Meaning that the substs_list instance
       takes repsonibility of freeing, _AND_ the calling scope is free
       to do whatever it wants with the value pointer.

*/
   
void subst_list_insert_ref(subst_list_type * subst_list , const char * key , const char * value) {
  subst_list_insert__(subst_list , key , value , SUBST_SHARED_REF);
}

void subst_list_insert_owned_ref(subst_list_type * subst_list , const char * key , const char * value) {
  subst_list_insert__(subst_list , key , value , SUBST_MANAGED_REF);
}

void subst_list_insert_copy(subst_list_type * subst_list , const char * key , const char * value) {
  subst_list_insert__(subst_list , key , value , SUBST_DEEP_COPY);
}


void subst_list_insert_callback(subst_list_type * subst_list , const char * key , subst_callback_ftype * callback , void * callback_arg , free_ftype * free_callback_arg) {
  subst_list_string_type * node = subst_list_get_node(subst_list , key);
  if (node == NULL) /* Did not have the node. */
    node = subst_list_insert_new_node(subst_list , key);
  subst_list_node_set_callback( node , callback , callback_arg , free_callback_arg );
}



void subst_list_free(subst_list_type * subst_list) {
  vector_free( subst_list->string_data );
  vector_free( subst_list->func_data );
  free(subst_list);
}



static void subst_list_inplace_update_buffer____(const subst_list_type * subst_list , buffer_type * buffer) {
  int index;
  for (index = 0; index < vector_get_size( subst_list->string_data ); index++) {
    const subst_list_string_type * node = vector_iget_const( subst_list->string_data , index );
    const int shift = strlen( node->value ) - strlen( node->key );
    bool    match;
    buffer_rewind( buffer );
    do {
      match = buffer_strstr( buffer , node->key ); 
      if (match) {
        size_t offset = buffer_get_offset( buffer ) + strlen( node->key );
        if (shift != 0)
          buffer_memshift( buffer , offset , shift );
        
        /** Search continues at the end of the newly inserted string - i.e. no room for recursions. */
        buffer_fwrite( buffer , node->value , strlen( node->value ) , sizeof * node->value );
      }
    } while (match);
  }
  
  {
    parser_type     * parser     = parser_alloc( "," , "\"\'" , NULL , " \t" , NULL , NULL );
    {
      const char      * func_name  = "ADD";
      subst_func_type * subst_func = subst_func_pool_get_func( subst_list->func_pool , func_name );
      
      bool match;
      buffer_rewind( buffer );
      do {
        size_t match_pos;
        match     = buffer_strstr( buffer , func_name );
        match_pos = buffer_get_offset( buffer );
        
        if (match) {
          bool   update     = false;
          char * arg_start  = buffer_get_data( buffer );
          arg_start        += buffer_get_offset( buffer ) + strlen( func_name );
          
          if (arg_start[0] == '(') {  /* We require that an opening paren follows immediately behind the function name. */
            char * arg_end = strchr( arg_start , ')');
            if (arg_end != NULL) {
              /* OK - we found an enclosing () pair. */
              char            * arg_content = util_alloc_substring_copy( &arg_start[1] , arg_end - arg_start - 1);
              stringlist_type * arg_list    = parser_tokenize_buffer( parser , arg_content , true);
              char            * func_eval   = subst_func_eval( subst_func , arg_list );
              int               old_len     = strlen(func_name) + strlen( arg_content) + 2;       

              if (func_eval != NULL) {
                buffer_memshift( buffer , match_pos + old_len , strlen( func_eval ) - old_len);
                buffer_fwrite( buffer , func_eval , strlen( func_eval ) , sizeof * func_eval );
                free( func_eval );
                update = true;
              }
              
              free( arg_content );
              stringlist_free( arg_list );
            } 
          } 
          
          if (!update) 
            buffer_fseek( buffer , match_pos + strlen( func_name ) , SEEK_SET);
        }
      } while (match);
    }
    parser_free( parser );
  }
}

/**
   This is the lowest level function, doing all the search/replace
   work (in this scope).
*/

static void subst_list_inplace_update_buffer__(const subst_list_type * subst_list , char ** buffer) {
  int index;
  for (index = 0; index < vector_get_size( subst_list->string_data ); index++) {
    const subst_list_string_type * node = vector_iget_const( subst_list->string_data , index );
    const char * value = node->value;
    if ((value != NULL) || (node->callback != NULL))
      util_string_replace_inplace( buffer , node->key , value , node->callback , node->callback_arg);
  }
}
    


/**
   This function reads the content of a file, and writes a new file
   where all substitutions in subst_list have been performed. Observe
   that target_file and src_file *CAN* point to the same file, in
   which case this will amount to an inplace update. In that case a
   backup file is written, and held, during the execution of the
   function.
*/


void subst_list_filter_file(const subst_list_type * subst_list , const char * src_file , const char * target_file) {
  char * buffer;
  char * backup_file = NULL;
  if (util_same_file(src_file , target_file)) {
    char * backup_prefix = util_alloc_sprintf("%s-%s" , src_file , __func__);
    backup_file = util_alloc_tmp_file("/tmp" , backup_prefix , false);
    free(backup_prefix);
  }
  buffer = util_fread_alloc_file_content( src_file ,  NULL);
  /* Writing backup file */
  if (backup_file != NULL) {
    FILE * stream = util_fopen(backup_file , "w");
    util_fwrite( buffer , 1 , strlen(buffer) , stream , __func__);
    fclose(stream);
  }

  /* Doing the actual update */
  subst_list_inplace_update_buffer__(subst_list , &buffer);
  
  /* Writing updated file */
  {
    FILE * stream = util_fopen(target_file , "w");
    util_fwrite( buffer , 1 , strlen(buffer) , stream , __func__);
    fclose(stream);
  }

  /* OK - all went hunka dory - unlink the backup file and leave the building. */
  if (backup_file != NULL) {
    unlink( backup_file );
    free( backup_file );
  }
  free(buffer);
}


/**
   This function does search-replace on a file inplace.
*/
void subst_list_update_file(const subst_list_type * subst_list , const char * file) {
  subst_list_filter_file( subst_list , file , file );
}


/**
   This function does search-replace on string instance inplace.
*/
void subst_list_update_string(const subst_list_type * subst_list , char ** string) {
  subst_list_inplace_update_buffer__(subst_list , string);
}


void subst_list_update_buffer( const subst_list_type * subst_list , buffer_type * buffer ) {
  subst_list_inplace_update_buffer____( subst_list , buffer );
}



void subst_list_fprintf(const subst_list_type * subst_list , FILE * stream) {
  int index;
  for (index=0; index < vector_get_size( subst_list->string_data ); index++) {
    const subst_list_string_type * node = vector_iget_const( subst_list->string_data , index );
    fprintf(stream , "%s = %s\n" , node->key , node->value);
  }
}


/**
   This function allocates a new string where the search-replace
   operation has been performed.
*/

char * subst_list_alloc_filtered_string(const subst_list_type * subst_list , const char * string) {
  char * buffer = util_alloc_string_copy( string );
  subst_list_inplace_update_buffer__(subst_list , &buffer);
  return buffer;
}



/**
   This function writes a filtered version of string to a file.
*/
void subst_list_filtered_fprintf(const subst_list_type * subst_list , const char * string , FILE * stream) {
  char * copy = util_alloc_string_copy(string);
  subst_list_inplace_update_buffer__(subst_list , &copy);
  fprintf(stream , "%s" , copy);  
  free(copy);
}



/**
   This allocates a new subst_list instance, the copy process is deep,
   in the sense that all srings inserted in the new subst_list
   instance have their own storage, irrespective of the ownership in
   the original subst_list instance.
*/

subst_list_type * subst_list_alloc_deep_copy(const subst_list_type * src) {
  int index;
  subst_list_type * copy = subst_list_alloc( src->func_pool );
  for (index = 0; index < vector_get_size( src->string_data ); index++) {
    const subst_list_string_type * node = vector_iget_const( src->string_data , index );
    subst_list_insert__( copy , node->key , node->value , SUBST_DEEP_COPY);
  }
  return copy;
}


int subst_list_get_size( const subst_list_type * subst_list) {
  return vector_get_size(subst_list->string_data);
}



const char * subst_list_iget_key( const subst_list_type * subst_list , int index) {
  if (index < vector_get_size(subst_list->string_data)) {
    const subst_list_string_type * node = vector_iget_const( subst_list->string_data , index );
    return node->key;
  } else {
    util_abort("%s: index:%d to large \n",__func__ , index);
    return NULL;
  }
}


const char * subst_list_iget_value( const subst_list_type * subst_list , int index) {
  if (index < vector_get_size(subst_list->string_data)) {
    const subst_list_string_type * node = vector_iget_const( subst_list->string_data , index );
    return node->value;
  } else {
    util_abort("%s: index:%d to large \n",__func__ , index);
    return NULL;
  }
}



/**
   This function will perform subst-based substitution on all the
   elements in the stringlist. The stringlist is modified in place,
   and the following applies to memory:

    o Elements inserted with _ref() are just dropped on the floor,
      they were the responsability of the calling scope anyway.

    o Elements inserted with _owned_ref() are freed - INVALIDATING A
      POSSIBLE POINTER IN THE CALLING SCOPE.

    o The newly inserted element is inserted as _owned_ref() -
      i.e. with a destructor.
*/


void stringlist_apply_subst(stringlist_type * stringlist , const subst_list_type * subst_list) {
  int i;
  for (i=0; i < stringlist_get_size( stringlist ); i++) {
    const char * old_string = stringlist_iget( stringlist , i );
    char * new_string = subst_list_alloc_filtered_string( subst_list , old_string );
    stringlist_iset_owned_ref( stringlist , i , new_string );
  }
}

/*****************************************************************/
