/* Copyright 2017 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Included from legion.h - do not include this directly

// Useful for IDEs 
#include "legion.h"

namespace Legion {

    /**
     * \struct SerdezRedopFns
     * Small helper class for storing instantiated templates
     */
    struct SerdezRedopFns {
    public:
      SerdezInitFnptr init_fn;
      SerdezFoldFnptr fold_fn;
    };

    /**
     * \class LegionSerialization
     * The Legion serialization class provides template meta-programming
     * help for returning complex data types from task calls.  If the 
     * types have three special methods defined on them then we know
     * how to serialize the type for the runtime rather than just doing
     * a dumb bit copy.  This is especially useful for types which 
     * require deep copies instead of shallow bit copies.  The three
     * methods which must be defined are:
     * size_t legion_buffer_size(void)
     * void legion_serialize(void *buffer)
     * void legion_deserialize(const void *buffer)
     */
    class LegionSerialization {
    public:
      // A helper method for getting access to the runtime's
      // end_task method with private access
      static inline void end_helper(Runtime *rt, InternalContext ctx,
          const void *result, size_t result_size, bool owned)
      {
        ctx->end_task(result, result_size, owned);
      }
      static inline Future from_value_helper(Runtime *rt, 
          const void *value, size_t value_size, bool owned)
      {
        return rt->from_value(value, value_size, owned);
      }

      // WARNING: There are two levels of SFINAE (substitution failure is 
      // not an error) here.  Proceed at your own risk. First we have to 
      // check to see if the type is a struct.  If it is then we check to 
      // see if it has a 'legion_serialize' method.  We assume if there is 
      // a 'legion_serialize' method there are also 'legion_buffer_size'
      // and 'legion_deserialize' methods.
      
      template<typename T, bool HAS_SERIALIZE>
      struct NonPODSerializer {
        static inline void end_task(Runtime *rt, InternalContext ctx,
                                    T *result)
        {
          size_t buffer_size = result->legion_buffer_size();
          void *buffer = malloc(buffer_size);
          result->legion_serialize(buffer);
          end_helper(rt, ctx, buffer, buffer_size, true/*owned*/);
          // No need to free the buffer, the Legion runtime owns it now
        }
        static inline Future from_value(Runtime *rt, const T *value)
        {
          size_t buffer_size = value->legion_buffer_size();
          void *buffer = malloc(buffer_size);
          value->legion_serialize(buffer);
          return from_value_helper(rt, buffer, buffer_size, true/*owned*/);
        }
        static inline T unpack(const void *result)
        {
          T derez;
          derez.legion_deserialize(result);
          return derez;
        }
      };

      template<typename T>
      struct NonPODSerializer<T,false> {
        static inline void end_task(Runtime *rt, InternalContext ctx,
                                    T *result)
        {
          end_helper(rt, ctx, (void*)result, sizeof(T), false/*owned*/);
        }
        static inline Future from_value(Runtime *rt, const T *value)
        {
          return from_value_helper(rt, (const void*)value,
                                   sizeof(T), false/*owned*/);
        }
        static inline T unpack(const void *result)
        {
          return (*((const T*)result));
        }
      };

      template<typename T>
      struct HasSerialize {
        typedef char no[1];
        typedef char yes[2];

        struct Fallback { void legion_serialize(void *); };
        struct Derived : T, Fallback { };

        template<typename U, U> struct Check;

        template<typename U>
        static no& test_for_serialize(
                  Check<void (Fallback::*)(void*), &U::legion_serialize> *);

        template<typename U>
        static yes& test_for_serialize(...);

        static const bool value = 
          (sizeof(test_for_serialize<Derived>(0)) == sizeof(yes));
      };

      template<typename T, bool IS_STRUCT>
      struct StructHandler {
        static inline void end_task(Runtime *rt, 
                                    InternalContext ctx, T *result)
        {
          // Otherwise this is a struct, so see if it has serialization methods
          NonPODSerializer<T,HasSerialize<T>::value>::end_task(rt, ctx, result);
        }
        static inline Future from_value(Runtime *rt, const T *value)
        {
          return NonPODSerializer<T,HasSerialize<T>::value>::from_value(
                                                                  rt, value);
        }
        static inline T unpack(const void *result)
        {
          return NonPODSerializer<T,HasSerialize<T>::value>::unpack(result); 
        }
      };
      // False case of template specialization
      template<typename T>
      struct StructHandler<T,false> {
        static inline void end_task(Runtime *rt, InternalContext ctx, 
                                    T *result)
        {
          end_helper(rt, ctx, (void*)result, sizeof(T), false/*owned*/);
        }
        static inline Future from_value(Runtime *rt, const T *value)
        {
          return from_value_helper(rt, (const void*)value, 
                                   sizeof(T), false/*owned*/);
        }
        static inline T unpack(const void *result)
        {
          return (*((const T*)result));
        }
      };

      template<typename T>
      struct IsAStruct {
        typedef char no[1];
        typedef char yes[2];
        
        template <typename U> static yes& test_for_struct(int U:: *x);
        template <typename U> static no& test_for_struct(...);

        static const bool value = 
                        (sizeof(test_for_struct<T>(0)) == sizeof(yes));
      };

      // Figure out whether this is a struct or not 
      // and call the appropriate Finisher
      template<typename T>
      static inline void end_task(Runtime *rt, InternalContext ctx, T *result)
      {
        StructHandler<T,IsAStruct<T>::value>::end_task(rt, ctx, result);
      }

      template<typename T>
      static inline Future from_value(Runtime *rt, const T *value)
      {
        return StructHandler<T,IsAStruct<T>::value>::from_value(rt, value);
      }

      template<typename T>
      static inline T unpack(const void *result)
      {
        return StructHandler<T,IsAStruct<T>::value>::unpack(result);
      }

      // Some more help for reduction operations with RHS types
      // that have serialize and deserialize methods

      template<typename REDOP_RHS>
      static void serdez_redop_init(const ReductionOp *reduction_op,
                              void *&ptr, size_t &size)
      {
        REDOP_RHS init_serdez;
        reduction_op->init(&init_serdez, 1);
        size_t new_size = init_serdez.legion_buffer_size();
        if (new_size > size)
        {
          size = new_size;
          ptr = realloc(ptr, size);
        }
        init_serdez.legion_serialize(ptr);
      }

      template<typename REDOP_RHS>
      static void serdez_redop_fold(const ReductionOp *reduction_op,
                                    void *&lhs_ptr, size_t &lhs_size,
                                    const void *rhs_ptr)
      {
        REDOP_RHS lhs_serdez, rhs_serdez;
        lhs_serdez.legion_deserialize(lhs_ptr);
        rhs_serdez.legion_deserialize(rhs_ptr);
        reduction_op->fold(&lhs_serdez, &rhs_serdez, 1, true/*exclusive*/);
        size_t new_size = lhs_serdez.legion_buffer_size();
        // Reallocate the buffer if it has grown
        if (new_size > lhs_size)
        {
          lhs_size = new_size;
          lhs_ptr = realloc(lhs_ptr, lhs_size);
        }
        // Now save the value
        lhs_serdez.legion_serialize(lhs_ptr);
      }

      template<typename REDOP_RHS, bool HAS_SERDEZ>
      struct SerdezRedopHandler {
        static inline void register_reduction(SerdezRedopTable &table,
                                              ReductionOpID redop_id)
        {
          // Do nothing in the case where there are no serdez functions
        }
      };
      // True case of template specialization
      template<typename REDOP_RHS>
      struct SerdezRedopHandler<REDOP_RHS,true> {
        static inline void register_reduction(SerdezRedopTable &table,
                                              ReductionOpID redop_id)
        {
          // Now we can do the registration
          SerdezRedopFns &fns = table[redop_id];
          fns.init_fn = serdez_redop_init<REDOP_RHS>;
          fns.fold_fn = serdez_redop_fold<REDOP_RHS>;
        }
      };

      template<typename REDOP_RHS, bool IS_STRUCT>
      struct StructRedopHandler {
        static inline void register_reduction(SerdezRedopTable &table,
                                              ReductionOpID redop_id)
        {
          // Do nothing in the case where this isn't a struct
        }
      };
      // True case of template specialization
      template<typename REDOP_RHS>
      struct StructRedopHandler<REDOP_RHS,true> {
        static inline void register_reduction(SerdezRedopTable &table,
                                              ReductionOpID redop_id)
        {
          SerdezRedopHandler<REDOP_RHS,HasSerialize<REDOP_RHS>::value>::
            register_reduction(table, redop_id);
        }
      };

      // Register reduction functions if necessary
      template<typename REDOP>
      static inline void register_reduction(SerdezRedopTable &table,
                                            ReductionOpID redop_id)
      {
        StructRedopHandler<typename REDOP::RHS, 
          IsAStruct<typename REDOP::RHS>::value>::register_reduction(table, 
                                                                     redop_id);
      }

    };

    // Special namespace for providing multi-dimensional 
    // array syntax on accessors 
    namespace AccessorHelp {
      // A small helper class that helps provide some syntactic sugar for
      // indexing accessors like a multi-dimensional array 
      template<typename A, typename FT, int N, typename T, 
                int M, bool READ_ONLY>
      class ArraySyntaxHelper {
      public:
        __CUDA_HD__
        ArraySyntaxHelper(const A &acc, const Realm::ZPoint<M-1,T> &p)
          : accessor(acc)
        {
          for (int i = 0; i < (M-1); i++)
            point[i] = p[i];
        }
      public:
        __CUDA_HD__
        inline ArraySyntaxHelper<A,FT,N,T,M+1,READ_ONLY> operator[](T val)
        {
          point[M-1] = val;
          return ArraySyntaxHelper<A,FT,N,T,M+1,READ_ONLY>(accessor, point);
        }
      public:
        const A &accessor;
        Realm::ZPoint<M,T> point;
      };
      // Specialization for M = N
      template<typename A, typename FT, int N, typename T, bool RO>
      class ArraySyntaxHelper<A,FT,N,T,N,RO> {
      public:
        __CUDA_HD__
        ArraySyntaxHelper(const A &acc, const Realm::ZPoint<N-1,T> &p)
          : accessor(acc)
        {
          for (int i = 0; i < (N-1); i++)
            point[i] = p[i];
        }
      public:
        __CUDA_HD__
        inline FT& operator[](T val)
        {
          point[N-1] = val;
          return accessor[point];
        }
      public:
        const A &accessor;
        Realm::ZPoint<N,T> point;
      };
      // Further specialization for M = N and read-only
      template<typename A, typename FT, int N, typename T>
      class ArraySyntaxHelper<A,FT,N,T,N,true> {
      public:
        __CUDA_HD__
        ArraySyntaxHelper(const A &acc, const Realm::ZPoint<N-1,T> &p)
          : accessor(acc)
        {
          for (int i = 0; i < (N-1); i++)
            point[i] = p[i];
        }
      public:
        __CUDA_HD__
        inline const FT& operator[](T val)
        {
          point[N-1] = val;
          return accessor[point];
        }
      public:
        const A &accessor;
        Realm::ZPoint<N,T> point;
      };
    };

    // Read-only FieldAccessor specialization
    template<typename FT, int N, typename T, typename A, bool CB>
    class FieldAccessor<READ_ONLY,FT,N,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_ONLY, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const 
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline const FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline const FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<READ_ONLY,FT,N,T,A>,FT,N,T,2,true/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<READ_ONLY,FT,N,T,A>,FT,N,T,2,true/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
    public:
      A accessor;
    };

    // Read-only FieldAccessor specialization
    // with bounds checks
    template<typename FT, int N, typename T, typename A>
    class FieldAccessor<READ_ONLY,FT,N,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<N,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_ONLY, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const 
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline const FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline const FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<READ_ONLY,FT,N,T,A>,FT,N,T,2,true/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<READ_ONLY,FT,N,T,A>,FT,N,T,2,true/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<N,T> bounds;
      mutable bool gpu_warning;
    };

    // Read-only FieldAccessor specialization 
    // with N==1 to avoid array ambiguity
    template<typename FT, typename T, typename A, bool CB>
    class FieldAccessor<READ_ONLY,FT,1,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_ONLY, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const 
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline const FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline const FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
          return accessor[p]; 
        }
    public:
      A accessor;
    };

    // Read-only FieldAccessor specialization 
    // with N==1 to avoid array ambiguity and bounds checks
    template<typename FT, typename T, typename A>
    class FieldAccessor<READ_ONLY,FT,1,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<1,T>()), 
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_ONLY, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const 
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline const FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline const FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor[p]; 
        }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<1,T> bounds;
      mutable bool gpu_warning;
    };

    // Read-write FieldAccessor specialization
    template<typename FT, int N, typename T, typename A, bool CB>
    class FieldAccessor<READ_WRITE,FT,N,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_WRITE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<N,T>& p, FT val) const
        { 
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<READ_WRITE,FT,N,T,A>,FT,N,T,2,false/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<READ_WRITE,FT,N,T,A>,FT,N,T,2,false/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__
      inline void reduce(const Realm::ZPoint<N,T>& p, 
                         typename REDOP::RHS val) const
        { 
          REDOP::template apply<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
    };

    // Read-write FieldAccessor specialization
    // with bounds checks
    template<typename FT, int N, typename T, typename A>
    class FieldAccessor<READ_WRITE,FT,N,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<N,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_WRITE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<N,T>& p, FT val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field,WRITE_DISCARD);
#endif
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<READ_WRITE,FT,N,T,A>,FT,N,T,2,false/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<READ_WRITE,FT,N,T,A>,FT,N,T,2,false/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__ 
      inline void reduce(const Realm::ZPoint<N,T>& p, 
                         typename REDOP::RHS val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, REDUCE);
#endif
          REDOP::template apply<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<N,T> bounds;
      mutable bool gpu_warning;
    };

    // Read-write FieldAccessor specialization 
    // with N==1 to avoid array ambiguity
    template<typename FT, typename T, typename A, bool CB>
    class FieldAccessor<READ_WRITE,FT,1,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_WRITE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<1,T>& p, FT val) const
        { 
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
          return accessor[p]; 
        }
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__
      inline void reduce(const Realm::ZPoint<1,T>& p, 
                         typename REDOP::RHS val) const
        { 
          REDOP::template apply<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
    };

    // Read-write FieldAccessor specialization 
    // with N==1 to avoid array ambiguity and bounds checks
    template<typename FT, typename T, typename A>
    class FieldAccessor<READ_WRITE,FT,1,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<1,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(READ_WRITE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<1,T>& p, FT val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field,WRITE_DISCARD);
#endif
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor[p]; 
        }
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__
      inline void reduce(const Realm::ZPoint<1,T>& p, 
                         typename REDOP::RHS val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, REDUCE);
#endif
          REDOP::template apply<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<1,T> bounds;
      mutable bool gpu_warning;
    };

    // Write-discard FieldAccessor specialization
    template<typename FT, int N, typename T, typename A, bool CB>
    class FieldAccessor<WRITE_DISCARD,FT,N,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(WRITE_DISCARD, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<N,T>& p, FT val) const
        { 
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<WRITE_DISCARD,FT,N,T,A>,FT,N,T,2,false/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<WRITE_DISCARD,FT,N,T,A>,FT,N,T,2,false/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
    public:
      A accessor;
    };

    // Write-discard FieldAccessor specialization
    // with bounds checks
    template<typename FT, int N, typename T, typename A>
    class FieldAccessor<WRITE_DISCARD,FT,N,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<N,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(WRITE_DISCARD, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<N,T>& p, FT val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field,WRITE_DISCARD);
#endif
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<N,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor[p]; 
        }
      __CUDA_HD__
      inline AccessorHelp::ArraySyntaxHelper<
        FieldAccessor<WRITE_DISCARD,FT,N,T,A>,FT,N,T,2,false/*read only*/>
          operator[](T index) const
      {
        return AccessorHelp::ArraySyntaxHelper<
          FieldAccessor<WRITE_DISCARD,FT,N,T,A>,FT,N,T,2,false/*read only*/>(
              *this, Realm::ZPoint<1,T>(index));
      }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<N,T> bounds;
      mutable bool gpu_warning;
    };

    // Write-discard FieldAccessor specialization with
    // N == 1 to avoid array ambiguity
    template<typename FT, typename T, typename A, bool CB>
    class FieldAccessor<WRITE_DISCARD,FT,1,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(WRITE_DISCARD, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const
        { 
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<1,T>& p, FT val) const
        { 
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
          return accessor[p]; 
        }
    public:
      A accessor;
    };

    // Write-discard FieldAccessor specialization with
    // N == 1 to avoid array ambiguity and bounds checks
    template<typename FT, typename T, typename A>
    class FieldAccessor<WRITE_DISCARD,FT,1,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<1,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(WRITE_DISCARD, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_ONLY);
#endif
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<1,T>& p, FT val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field,WRITE_DISCARD);
#endif
          accessor.write(p, val); 
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor.ptr(p); 
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<1,T>& p) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, READ_WRITE);
#endif
          return accessor[p]; 
        }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<1,T> bounds;
      mutable bool gpu_warning;
    };

    // Reduce FieldAccessor specialization
    template<typename FT, int N, typename T, typename A, bool CB>
    class FieldAccessor<REDUCE,FT,N,T,A,CB> {
    public:
      __CUDA_HD__
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    ReductionOpID redop, bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(REDUCE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), 
              silence_warnings, redop);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__
      inline void reduce(const Realm::ZPoint<N,T>& p, 
                         typename REDOP::RHS val) const
        { 
          REDOP::template fold<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
    };

    // Reduce FieldAccessor specialization with bounds checks
    template<typename FT, int N, typename T, typename A>
    class FieldAccessor<REDUCE,FT,N,T,A,true> {
    public:
      // No CUDA support due to PhysicalRegion constructor
      FieldAccessor(void) { }
      FieldAccessor(const PhysicalRegion &region, FieldID fid,
                    ReductionOpID redop, bool silence_warnings = false)
        : field(fid), field_region(region), 
          bounds(region.template get_bounds<N,T>()),
          gpu_warning(!silence_warnings)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(REDUCE, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), 
              silence_warnings, redop);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      template<typename REDOP, bool EXCLUSIVE> __CUDA_HD__ 
      inline void reduce(const Realm::ZPoint<N,T>& p, 
                         typename REDOP::RHS val) const
        { 
#ifdef __CUDA_ARCH__
          if (gpu_warning)
          {
            if (!bounds.dense())
              printf("WARNING: GPU bounds check is imprecise\n");
            gpu_warning = false;
          }
          assert(bounds.bounds.contains(p));
#else
          if (!bounds.contains(p)) 
            field_region.fail_bounds_check(DomainPoint(p), field, REDUCE);
#endif
          REDOP::template fold<EXCLUSIVE>(accessor[p], val);
        }
    public:
      A accessor;
      FieldID field;
      PhysicalRegion field_region;
      Realm::ZIndexSpace<N,T> bounds;
      mutable bool gpu_warning;
    };

    // A hidden class for users that really know what they are doing
    /**
     * \class UnsafeFieldAccessor
     * This is a class for getting access to region data without
     * privilege checks or bounds checks. Users should only use
     * this accessor if they are confident that they actually do
     * have their privileges and bounds correct
     */
    template<typename FT, int N, typename T = coord_t,
             typename A = Realm::AffineAccessor<FT,N,T> >
    class UnsafeFieldAccessor {
    public:
      UnsafeFieldAccessor(void) { }
      UnsafeFieldAccessor(const PhysicalRegion &region, FieldID fid,
                          bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<N,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(NO_ACCESS, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<N,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<N,T> &p) const
        {
          return accessor.read(p);
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<N,T> &p, FT val) const
        {
          accessor.write(p, val);
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<N,T> &p) const
        {
          return accessor.ptr(p);
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<N,T> &p) const
        {
          return accessor[p];
        }
      __CUDA_HD__
      inline FT& operator[](T index) const
        {
          return AccessorHelp::ArraySyntaxHelper<
            UnsafeFieldAccessor<FT,N,T,A>,FT,N,T,2,false/*read only*/>(
                *this, Realm::ZPoint<1,T>(index));
        }
    public:
      A accessor;
    };

    // Specialization for UnsafeFieldAccessor for dimension 1 
    // to avoid ambiguity for array access
    template<typename FT, typename T, typename A>
    class UnsafeFieldAccessor<FT,1,T,A> {
    public:
      UnsafeFieldAccessor(void) { }
      UnsafeFieldAccessor(const PhysicalRegion &region, FieldID fid,
                          bool silence_warnings = false)
      {
        ptrdiff_t field_offset; Realm::ZIndexSpace<1,T> is;
        const Realm::RegionInstance instance = 
          region.get_instance_info(NO_ACCESS, fid, field_offset, &is,
              Internal::NT_TemplateHelper::encode_tag<1,T>(), silence_warnings);
        accessor = A(instance, field_offset, is.bounds);
      }
    public:
      __CUDA_HD__
      inline FT read(const Realm::ZPoint<1,T> &p) const
        {
          return accessor.read(p); 
        }
      __CUDA_HD__
      inline void write(const Realm::ZPoint<1,T> &p, FT val) const
        {
          accessor.write(p, val);
        }
      __CUDA_HD__
      inline FT* ptr(const Realm::ZPoint<1,T> &p) const
        {
          return accessor.ptr(p);
        }
      __CUDA_HD__
      inline FT& operator[](const Realm::ZPoint<1,T> &p) const
        {
          return accessor[p];
        }
    public:
      A accessor;
    }; 

    //--------------------------------------------------------------------------
    inline IndexSpace& IndexSpace::operator=(const IndexSpace &rhs)
    //--------------------------------------------------------------------------
    {
      id = rhs.id;
      tid = rhs.tid;
      type_tag = rhs.type_tag;
      return *this;
    }

    //--------------------------------------------------------------------------
    inline bool IndexSpace::operator==(const IndexSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id != rhs.id)
        return false;
      if (tid != rhs.tid)
        return false;
#ifdef DEBUG_LEGION
      assert(type_tag == rhs.type_tag);
#endif
      return true;
    }

    //--------------------------------------------------------------------------
    inline bool IndexSpace::operator!=(const IndexSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      if ((id == rhs.id) && (tid == rhs.tid))
        return false;
      return true;
    }

    //--------------------------------------------------------------------------
    inline bool IndexSpace::operator<(const IndexSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id < rhs.id)
        return true;
      if (id > rhs.id)
        return false;
      return (tid < rhs.tid);
    }

    //--------------------------------------------------------------------------
    inline bool IndexSpace::operator>(const IndexSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id > rhs.id)
        return true;
      if (id < rhs.id)
        return false;
      return (tid > rhs.tid);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T>::IndexSpaceT(IndexSpaceID id, IndexTreeID tid)
      : IndexSpace(id, tid, 
          Internal::NT_TemplateHelper::template encode_tag<DIM,T>()) 
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T>::IndexSpaceT(void)
      : IndexSpace()
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T>::IndexSpaceT(const IndexSpaceT &rhs)
      : IndexSpace(rhs.get_id(), rhs.get_tree_id(), rhs.get_type_tag())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(type_tag);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T>::IndexSpaceT(const IndexSpace &rhs)
      : IndexSpace(rhs.get_id(), rhs.get_tree_id(), rhs.get_type_tag())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(type_tag);
    }

    //--------------------------------------------------------------------------
    inline IndexPartition& IndexPartition::operator=(const IndexPartition &rhs)
    //--------------------------------------------------------------------------
    {
      id = rhs.id;
      tid = rhs.tid;
      type_tag = rhs.type_tag;
      return *this;
    }
    
    //--------------------------------------------------------------------------
    inline bool IndexPartition::operator==(const IndexPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id != rhs.id)
        return false;
      if (tid != rhs.tid)
        return false;
#ifdef DEBUG_LEGION
      assert(type_tag == rhs.type_tag);
#endif
      return true;
    }

    //--------------------------------------------------------------------------
    inline bool IndexPartition::operator!=(const IndexPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      if ((id == rhs.id) && (tid == rhs.tid))
        return false;
      return true;
    }

    //--------------------------------------------------------------------------
    inline bool IndexPartition::operator<(const IndexPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id < rhs.id)
        return true;
      if (id > rhs.id)
        return false;
      return (tid < rhs.tid);
    }

    //--------------------------------------------------------------------------
    inline bool IndexPartition::operator>(const IndexPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      if (id > rhs.id)
        return true;
      if (id < rhs.id)
        return false;
      return (tid > rhs.tid);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T>::IndexPartitionT(IndexPartitionID id,IndexTreeID tid)
      : IndexPartition(id, tid,Internal::NT_TemplateHelper::encode_tag<DIM,T>())
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T>::IndexPartitionT(void)
      : IndexPartition()
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T>::IndexPartitionT(const IndexPartitionT &rhs)
      : IndexPartition(rhs.get_id(), rhs.get_tree_id(), rhs.get_type_tag())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(type_tag);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T>::IndexPartitionT(const IndexPartition &rhs)
      : IndexPartition(rhs.get_id(), rhs.get_tree_id(), rhs.get_type_tag())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(type_tag);
    }
    
    //--------------------------------------------------------------------------
    inline FieldSpace& FieldSpace::operator=(const FieldSpace &rhs)
    //--------------------------------------------------------------------------
    {
      id = rhs.id;
      return *this;
    }

    //--------------------------------------------------------------------------
    inline bool FieldSpace::operator==(const FieldSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      return (id == rhs.id);
    }

    //--------------------------------------------------------------------------
    inline bool FieldSpace::operator!=(const FieldSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      return (id != rhs.id);
    }

    //--------------------------------------------------------------------------
    inline bool FieldSpace::operator<(const FieldSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      return (id < rhs.id);
    }

    //--------------------------------------------------------------------------
    inline bool FieldSpace::operator>(const FieldSpace &rhs) const
    //--------------------------------------------------------------------------
    {
      return (id > rhs.id);
    }

    //--------------------------------------------------------------------------
    inline LogicalRegion& LogicalRegion::operator=(const LogicalRegion &rhs) 
    //--------------------------------------------------------------------------
    {
      tree_id = rhs.tree_id;
      index_space = rhs.index_space;
      field_space = rhs.field_space;
      return *this;
    }
    
    //--------------------------------------------------------------------------
    inline bool LogicalRegion::operator==(const LogicalRegion &rhs) const
    //--------------------------------------------------------------------------
    {
      return ((tree_id == rhs.tree_id) && (index_space == rhs.index_space) 
              && (field_space == rhs.field_space));
    }

    //--------------------------------------------------------------------------
    inline bool LogicalRegion::operator!=(const LogicalRegion &rhs) const
    //--------------------------------------------------------------------------
    {
      return (!((*this) == rhs));
    }

    //--------------------------------------------------------------------------
    inline bool LogicalRegion::operator<(const LogicalRegion &rhs) const
    //--------------------------------------------------------------------------
    {
      if (tree_id < rhs.tree_id)
        return true;
      else if (tree_id > rhs.tree_id)
        return false;
      else
      {
        if (index_space < rhs.index_space)
          return true;
        else if (index_space != rhs.index_space) // therefore greater than
          return false;
        else
          return field_space < rhs.field_space;
      }
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T>::LogicalRegionT(RegionTreeID tid, 
                                          IndexSpace is, FieldSpace fs)
      : LogicalRegion(tid, is, fs)
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                            is.get_type_tag());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T>::LogicalRegionT(void)
       : LogicalRegion()
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T>::LogicalRegionT(const LogicalRegionT &rhs)
      : LogicalRegion(rhs.get_tree_id(), rhs.get_index_space(), 
                      rhs.get_field_space())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                rhs.get_type_tag());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T>::LogicalRegionT(const LogicalRegion &rhs)
      : LogicalRegion(rhs.get_tree_id(), rhs.get_index_space(), 
                      rhs.get_field_space())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                rhs.get_type_tag());
    }

    //--------------------------------------------------------------------------
    inline LogicalPartition& LogicalPartition::operator=(
                                                    const LogicalPartition &rhs)
    //--------------------------------------------------------------------------
    {
      tree_id = rhs.tree_id;
      index_partition = rhs.index_partition;
      field_space = rhs.field_space;
      return *this;
    }

    //--------------------------------------------------------------------------
    inline bool LogicalPartition::operator==(const LogicalPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      return ((tree_id == rhs.tree_id) && 
              (index_partition == rhs.index_partition) && 
              (field_space == rhs.field_space));
    }

    //--------------------------------------------------------------------------
    inline bool LogicalPartition::operator!=(const LogicalPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      return (!((*this) == rhs));
    }

    //--------------------------------------------------------------------------
    inline bool LogicalPartition::operator<(const LogicalPartition &rhs) const
    //--------------------------------------------------------------------------
    {
      if (tree_id < rhs.tree_id)
        return true;
      else if (tree_id > rhs.tree_id)
        return false;
      else
      {
        if (index_partition < rhs.index_partition)
          return true;
        else if (index_partition > rhs.index_partition)
          return false;
        else
          return (field_space < rhs.field_space);
      }
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T>::LogicalPartitionT(RegionTreeID tid, 
                                              IndexPartition pid, FieldSpace fs)
      : LogicalPartition(tid, pid, fs)
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                            pid.get_type_tag());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T>::LogicalPartitionT(void)
      : LogicalPartition()
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T>::LogicalPartitionT(const LogicalPartitionT &rhs)
      : LogicalPartition(rhs.get_tree_id(), rhs.get_index_partition(), 
                         rhs.get_field_space())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                            rhs.get_type_tag());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T>::LogicalPartitionT(const LogicalPartition &rhs)
      : LogicalPartition(rhs.get_tree_id(), rhs.get_index_partition(), 
                         rhs.get_field_space())
    //--------------------------------------------------------------------------
    {
      Internal::NT_TemplateHelper::template check_type<DIM,T>(
                                            rhs.get_type_tag());
    }

    //--------------------------------------------------------------------------
    inline bool IndexAllocator::operator==(const IndexAllocator &rhs) const
    //--------------------------------------------------------------------------
    {
      return ((index_space == rhs.index_space) && (allocator == rhs.allocator));
    }

    //--------------------------------------------------------------------------
    inline bool IndexAllocator::operator<(const IndexAllocator &rhs) const
    //--------------------------------------------------------------------------
    {
      if (allocator < rhs.allocator)
        return true;
      else if (allocator > rhs.allocator)
        return false;
      else
        return (index_space < rhs.index_space);
    }

    //--------------------------------------------------------------------------
    inline ptr_t IndexAllocator::alloc(unsigned num_elements /*= 1*/)
    //--------------------------------------------------------------------------
    {
      ptr_t result(allocator->alloc(num_elements));
      return result;
    }

    //--------------------------------------------------------------------------
    inline void IndexAllocator::free(ptr_t ptr, unsigned num_elements /*= 1*/)
    //--------------------------------------------------------------------------
    {
      allocator->free(ptr.value,num_elements);
    }

    //--------------------------------------------------------------------------
    inline bool FieldAllocator::operator==(const FieldAllocator &rhs) const
    //--------------------------------------------------------------------------
    {
      return ((field_space == rhs.field_space) && (runtime == rhs.runtime));
    }

    //--------------------------------------------------------------------------
    inline bool FieldAllocator::operator<(const FieldAllocator &rhs) const
    //--------------------------------------------------------------------------
    {
      if (runtime < rhs.runtime)
        return true;
      else if (runtime > rhs.runtime)
        return false;
      else
        return (field_space < rhs.field_space);
    }

    //--------------------------------------------------------------------------
    inline FieldID FieldAllocator::allocate_field(size_t field_size, 
                                FieldID desired_fieldid /*= AUTO_GENERATE_ID*/,
                                CustomSerdezID serdez_id /*=0*/)
    //--------------------------------------------------------------------------
    {
      return runtime->allocate_field(parent, field_space, 
                                     field_size, desired_fieldid, 
                                     false/*local*/, serdez_id); 
    }

    //--------------------------------------------------------------------------
    inline void FieldAllocator::free_field(FieldID id)
    //--------------------------------------------------------------------------
    {
      runtime->free_field(parent, field_space, id);
    }

    //--------------------------------------------------------------------------
    inline FieldID FieldAllocator::allocate_local_field(size_t field_size,
                                FieldID desired_fieldid /*= AUTO_GENERATE_ID*/,
                                CustomSerdezID serdez_id /*=0*/)
    //--------------------------------------------------------------------------
    {
      return runtime->allocate_field(parent, field_space,
                                     field_size, desired_fieldid, 
                                     true/*local*/, serdez_id);
    }

    //--------------------------------------------------------------------------
    inline void FieldAllocator::allocate_fields(
        const std::vector<size_t> &field_sizes,
        std::vector<FieldID> &resulting_fields, CustomSerdezID serdez_id /*=0*/)
    //--------------------------------------------------------------------------
    {
      runtime->allocate_fields(parent, field_space, 
                               field_sizes, resulting_fields, 
                               false/*local*/, serdez_id);
    }

    //--------------------------------------------------------------------------
    inline void FieldAllocator::free_fields(const std::set<FieldID> &to_free)
    //--------------------------------------------------------------------------
    {
      runtime->free_fields(parent, field_space, to_free);
    }

    //--------------------------------------------------------------------------
    inline void FieldAllocator::allocate_local_fields(
        const std::vector<size_t> &field_sizes,
        std::vector<FieldID> &resulting_fields, CustomSerdezID serdez_id /*=0*/)
    //--------------------------------------------------------------------------
    {
      runtime->allocate_fields(parent, field_space, 
                               field_sizes, resulting_fields, 
                               true/*local*/, serdez_id);
    }

    //--------------------------------------------------------------------------
    template<typename PT, unsigned DIM>
    inline void ArgumentMap::set_point_arg(const PT point[DIM], 
                                           const TaskArgument &arg, 
                                           bool replace/*= false*/)
    //--------------------------------------------------------------------------
    {
      LEGION_STATIC_ASSERT(DIM <= DomainPoint::MAX_POINT_DIM);  
      DomainPoint dp;
      dp.dim = DIM;
      for (unsigned idx = 0; idx < DIM; idx++)
        dp.point_data[idx] = point[idx];
      set_point(dp, arg, replace);
    }

    //--------------------------------------------------------------------------
    template<typename PT, unsigned DIM>
    inline bool ArgumentMap::remove_point(const PT point[DIM])
    //--------------------------------------------------------------------------
    {
      LEGION_STATIC_ASSERT(DIM <= DomainPoint::MAX_POINT_DIM);
      DomainPoint dp;
      dp.dim = DIM;
      for (unsigned idx = 0; idx < DIM; idx++)
        dp.point_data[idx] = point[idx];
      return remove_point(dp);
    }

    //--------------------------------------------------------------------------
    inline bool Predicate::operator==(const Predicate &p) const
    //--------------------------------------------------------------------------
    {
      if (impl == NULL)
      {
        if (p.impl == NULL)
          return (const_value == p.const_value);
        else
          return false;
      }
      else
        return (impl == p.impl);
    }

    //--------------------------------------------------------------------------
    inline bool Predicate::operator<(const Predicate &p) const
    //--------------------------------------------------------------------------
    {
      if (impl == NULL)
      {
        if (p.impl == NULL)
          return (const_value < p.const_value);
        else
          return true;
      }
      else
        return (impl < p.impl);
    }

    //--------------------------------------------------------------------------
    inline bool Predicate::operator!=(const Predicate &p) const
    //--------------------------------------------------------------------------
    {
      return !(*this == p);
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator~(RegionFlags f)
    //--------------------------------------------------------------------------
    {
      return static_cast<RegionFlags>(~unsigned(f));
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator|(RegionFlags left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      return static_cast<RegionFlags>(unsigned(left) | unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator&(RegionFlags left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      return static_cast<RegionFlags>(unsigned(left) & unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator^(RegionFlags left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      return static_cast<RegionFlags>(unsigned(left) ^ unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator|=(RegionFlags &left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l |= r;
      return left = static_cast<RegionFlags>(l);
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator&=(RegionFlags &left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l &= r;
      return left = static_cast<RegionFlags>(l);
    }

    //--------------------------------------------------------------------------
    inline RegionFlags operator^=(RegionFlags &left, RegionFlags right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l ^= r;
      return left = static_cast<RegionFlags>(l);
    }

    //--------------------------------------------------------------------------
    inline RegionRequirement& RegionRequirement::add_field(FieldID fid, 
                                             bool instance/*= true*/)
    //--------------------------------------------------------------------------
    {
      privilege_fields.insert(fid);
      if (instance)
        instance_fields.push_back(fid);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline RegionRequirement& RegionRequirement::add_fields(
                      const std::vector<FieldID>& fids, bool instance/*= true*/)
    //--------------------------------------------------------------------------
    {
      privilege_fields.insert(fids.begin(), fids.end());
      if (instance)
        instance_fields.insert(instance_fields.end(), fids.begin(), fids.end());
      return *this;
    }

    //--------------------------------------------------------------------------
    inline RegionRequirement& RegionRequirement::add_flags(
                                                          RegionFlags new_flags)
    //--------------------------------------------------------------------------
    {
      flags |= new_flags;
      return *this;
    }

    //--------------------------------------------------------------------------
    inline void StaticDependence::add_field(FieldID fid)
    //--------------------------------------------------------------------------
    {
      dependent_fields.insert(fid);
    }

    //--------------------------------------------------------------------------
    inline IndexSpaceRequirement& TaskLauncher::add_index_requirement(
                                              const IndexSpaceRequirement &req)
    //--------------------------------------------------------------------------
    {
      index_requirements.push_back(req);
      return index_requirements.back();
    }

    //--------------------------------------------------------------------------
    inline RegionRequirement& TaskLauncher::add_region_requirement(
                                                  const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      region_requirements.push_back(req);
      return region_requirements.back();
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_field(unsigned idx, FieldID fid, bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < region_requirements.size());
#endif
      region_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_future(Future f)
    //--------------------------------------------------------------------------
    {
      futures.push_back(f);
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_wait_handshake(MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::set_predicate_false_future(Future f)
    //--------------------------------------------------------------------------
    {
      predicate_false_future = f;
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::set_predicate_false_result(TaskArgument arg)
    //--------------------------------------------------------------------------
    {
      predicate_false_result = arg;
    }

    //--------------------------------------------------------------------------
    inline void TaskLauncher::set_independent_requirements(bool independent)
    //--------------------------------------------------------------------------
    {
      independent_requirements = independent;
    }

    //--------------------------------------------------------------------------
    inline IndexSpaceRequirement& IndexTaskLauncher::add_index_requirement(
                                              const IndexSpaceRequirement &req)
    //--------------------------------------------------------------------------
    {
      index_requirements.push_back(req);
      return index_requirements.back();
    }

    //--------------------------------------------------------------------------
    inline RegionRequirement& IndexTaskLauncher::add_region_requirement(
                                                  const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      region_requirements.push_back(req);
      return region_requirements.back();
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_field(unsigned idx,FieldID fid,bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < region_requirements.size());
#endif
      region_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_future(Future f)
    //--------------------------------------------------------------------------
    {
      futures.push_back(f);
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_wait_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::set_predicate_false_future(Future f)
    //--------------------------------------------------------------------------
    {
      predicate_false_future = f;
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::set_predicate_false_result(TaskArgument arg)
    //--------------------------------------------------------------------------
    {
      predicate_false_result = arg;
    }

    //--------------------------------------------------------------------------
    inline void IndexTaskLauncher::set_independent_requirements(
                                                               bool independent)
    //--------------------------------------------------------------------------
    {
      independent_requirements = independent;
    }

    //--------------------------------------------------------------------------
    inline void InlineLauncher::add_field(FieldID fid, bool inst)
    //--------------------------------------------------------------------------
    {
      requirement.add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline unsigned CopyLauncher::add_copy_requirements(
                     const RegionRequirement &src, const RegionRequirement &dst)
    //--------------------------------------------------------------------------
    {
      unsigned result = src_requirements.size();
#ifdef DEBUG_LEGION
      assert(result == dst_requirements.size());
#endif
      src_requirements.push_back(src);
      dst_requirements.push_back(dst);
      return result;
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_src_field(unsigned idx,FieldID fid,bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < src_requirements.size());
#endif
      src_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_dst_field(unsigned idx,FieldID fid,bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < dst_requirements.size());
#endif
      dst_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_wait_handshake(MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void CopyLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline unsigned IndexCopyLauncher::add_copy_requirements(
                     const RegionRequirement &src, const RegionRequirement &dst)
    //--------------------------------------------------------------------------
    {
      unsigned result = src_requirements.size();
#ifdef DEBUG_LEGION
      assert(result == dst_requirements.size());
#endif
      src_requirements.push_back(src);
      dst_requirements.push_back(dst);
      return result;
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_src_field(unsigned idx,
                                                 FieldID fid, bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < src_requirements.size());
#endif
      src_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_dst_field(unsigned idx,
                                                 FieldID fid, bool inst)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < dst_requirements.size());
#endif
      dst_requirements[idx].add_field(fid, inst);
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_wait_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void IndexCopyLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_field(FieldID f)
    //--------------------------------------------------------------------------
    {
      fields.insert(f);
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_wait_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void AcquireLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_field(FieldID f)
    //--------------------------------------------------------------------------
    {
      fields.insert(f);
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_wait_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      wait_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_arrival_barrier(PhaseBarrier bar)
    //--------------------------------------------------------------------------
    {
      assert(bar.exists());
      arrive_barriers.push_back(bar);
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_wait_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void ReleaseLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::set_argument(TaskArgument arg)
    //--------------------------------------------------------------------------
    {
      argument = arg;
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::set_future(Future f)
    //--------------------------------------------------------------------------
    {
      future = f;
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_field(FieldID fid)
    //--------------------------------------------------------------------------
    {
      fields.insert(fid);
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_wait_barrier(PhaseBarrier pb)
    //--------------------------------------------------------------------------
    {
      assert(pb.exists());
      wait_barriers.push_back(pb);
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_arrival_barrier(PhaseBarrier pb)
    //--------------------------------------------------------------------------
    {
      assert(pb.exists());
      arrive_barriers.push_back(pb);
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_wait_handshake(MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void FillLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::set_argument(TaskArgument arg)
    //--------------------------------------------------------------------------
    {
      argument = arg;
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::set_future(Future f)
    //--------------------------------------------------------------------------
    {
      future = f;
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_field(FieldID fid)
    //--------------------------------------------------------------------------
    {
      fields.insert(fid);
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_grant(Grant g)
    //--------------------------------------------------------------------------
    {
      grants.push_back(g);
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_wait_barrier(PhaseBarrier pb)
    //--------------------------------------------------------------------------
    {
      assert(pb.exists());
      wait_barriers.push_back(pb);
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_arrival_barrier(PhaseBarrier pb)
    //--------------------------------------------------------------------------
    {
      assert(pb.exists());
      arrive_barriers.push_back(pb);
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_wait_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      wait_barriers.push_back(handshake.get_legion_wait_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void IndexFillLauncher::add_arrival_handshake(
                                                   MPILegionHandshake handshake)
    //--------------------------------------------------------------------------
    {
      arrive_barriers.push_back(handshake.get_legion_arrive_phase_barrier());
    }

    //--------------------------------------------------------------------------
    inline void AttachLauncher::attach_file(const char *name,
                                            const std::vector<FieldID> &fields,
                                            LegionFileMode m)
    //--------------------------------------------------------------------------
    {
      file_name = name;
      mode = m;
      file_fields = fields;
    }

    //--------------------------------------------------------------------------
    inline void AttachLauncher::attach_hdf5(const char *name,
                                const std::map<FieldID,const char*> &field_map,
                                LegionFileMode m)
    //--------------------------------------------------------------------------
    {
      file_name = name;
      mode = m;
      field_files = field_map;
    }

    //--------------------------------------------------------------------------
    inline void AttachLauncher::add_field_pointer(FieldID fid, void *ptr)
    //--------------------------------------------------------------------------
    {
      field_pointers[fid] = ptr;
    }

    //--------------------------------------------------------------------------
    inline void AttachLauncher::set_pitch(unsigned dim, size_t pitch)
    //--------------------------------------------------------------------------
    {
      if (pitches.size() <= dim)
        pitches.resize(dim+1, 0);
      pitches[dim] = pitch;
    }

    //--------------------------------------------------------------------------
    inline void PredicateLauncher::add_predicate(const Predicate &pred)
    //--------------------------------------------------------------------------
    {
      predicates.push_back(pred);
    }

    //--------------------------------------------------------------------------
    inline void TimingLauncher::add_precondition(const Future &f)
    //--------------------------------------------------------------------------
    {
      preconditions.insert(f);
    }

    //--------------------------------------------------------------------------
    inline void MustEpochLauncher::add_single_task(const DomainPoint &point,
                                                   const TaskLauncher &launcher)
    //--------------------------------------------------------------------------
    {
      single_tasks.push_back(launcher);
      single_tasks.back().point = point;
    }

    //--------------------------------------------------------------------------
    inline void MustEpochLauncher::add_index_task(
                                              const IndexTaskLauncher &launcher)
    //--------------------------------------------------------------------------
    {
      index_tasks.push_back(launcher);
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                         add_constraint(const SpecializedConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                              add_constraint(const MemoryConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                            add_constraint(const OrderingConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                           add_constraint(const SplittingConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                               add_constraint(const FieldConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                           add_constraint(const DimensionConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                           add_constraint(const AlignmentConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                              add_constraint(const OffsetConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline LayoutConstraintRegistrar& LayoutConstraintRegistrar::
                             add_constraint(const PointerConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
                                 add_constraint(const ISAConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      execution_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
                           add_constraint(const ProcessorConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      execution_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
                            add_constraint(const ResourceConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      execution_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
                              add_constraint(const LaunchConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      execution_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
                          add_constraint(const ColocationConstraint &constraint)
    //--------------------------------------------------------------------------
    {
      execution_constraints.add_constraint(constraint);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline TaskVariantRegistrar& TaskVariantRegistrar::
             add_layout_constraint_set(unsigned index, LayoutConstraintID desc)
    //--------------------------------------------------------------------------
    {
      layout_constraints.add_layout_constraint(index, desc);
      return *this;
    }

    //--------------------------------------------------------------------------
    inline void TaskVariantRegistrar::set_leaf(bool is_leaf /*= true*/)
    //--------------------------------------------------------------------------
    {
      leaf_variant = is_leaf;
    }

    //--------------------------------------------------------------------------
    inline void TaskVariantRegistrar::set_inner(bool is_inner /*= true*/)
    //--------------------------------------------------------------------------
    {
      inner_variant = is_inner;
    }

    //--------------------------------------------------------------------------
    inline void TaskVariantRegistrar::set_idempotent(bool is_idemp/*= true*/)
    //--------------------------------------------------------------------------
    {
      idempotent_variant = is_idemp;
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline T Future::get_result(bool silence_warnings) const
    //--------------------------------------------------------------------------
    {
      // Unpack the value using LegionSerialization in case
      // the type has an alternative method of unpacking
      return 
        LegionSerialization::unpack<T>(get_untyped_result(silence_warnings));
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline const T& Future::get_reference(bool silence_warnings)
    //--------------------------------------------------------------------------
    {
      return *((const T*)get_untyped_result(silence_warnings));
    }

    //--------------------------------------------------------------------------
    inline const void* Future::get_untyped_pointer(bool silence_warnings)
    //--------------------------------------------------------------------------
    {
      return get_untyped_result(silence_warnings);
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline T Future::get(void)
    //--------------------------------------------------------------------------
    {
      return get_result<T>();
    }

    //--------------------------------------------------------------------------
    inline bool Future::valid(void) const
    //--------------------------------------------------------------------------
    {
      return (impl != NULL);
    }

    //--------------------------------------------------------------------------
    inline void Future::wait(void) const
    //--------------------------------------------------------------------------
    {
      get_void_result();
    }

    //--------------------------------------------------------------------------
    template<typename T>
    /*static*/ inline Future Future::from_value(Runtime *rt, const T &value)
    //--------------------------------------------------------------------------
    {
      return LegionSerialization::from_value(rt, &value);
    }

    //--------------------------------------------------------------------------
    /*static*/ inline Future Future::from_untyped_pointer(Runtime *rt,
							  const void *buffer,
							  size_t bytes)
    //--------------------------------------------------------------------------
    {
      return LegionSerialization::from_value_helper(rt, buffer, bytes,
						    false /*!owned*/);
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline T FutureMap::get_result(const DomainPoint &dp, bool silence_warnings)
    //--------------------------------------------------------------------------
    {
      Future f = get_future(dp);
      return f.get_result<T>(silence_warnings);
    }

    //--------------------------------------------------------------------------
    template<typename RT, typename PT, unsigned DIM>
    inline RT FutureMap::get_result(const PT point[DIM])
    //--------------------------------------------------------------------------
    {
      LEGION_STATIC_ASSERT(DIM <= DomainPoint::MAX_POINT_DIM);
      DomainPoint dp;
      dp.dim = DIM;
      for (unsigned idx = 0; idx < DIM; idx++)
        dp.point_data[idx] = point[idx];
      Future f = get_future(dp);
      return f.get_result<RT>();
    }

    //--------------------------------------------------------------------------
    template<typename PT, unsigned DIM>
    inline Future FutureMap::get_future(const PT point[DIM])
    //--------------------------------------------------------------------------
    {
      LEGION_STATIC_ASSERT(DIM <= DomainPoint::MAX_POINT_DIM);
      DomainPoint dp;
      dp.dim = DIM;
      for (unsigned idx = 0; idx < DIM; idx++)
        dp.point_data[idx] = point[idx];
      return get_future(dp);
    }

    //--------------------------------------------------------------------------
    template<typename PT, unsigned DIM>
    inline void FutureMap::get_void_result(const PT point[DIM])
    //--------------------------------------------------------------------------
    {
      LEGION_STATIC_ASSERT(DIM <= DomainPoint::MAX_POINT_DIM);
      DomainPoint dp;
      dp.dim = DIM;
      for (unsigned idx = 0; idx < DIM; idx++)
        dp.point_data[idx] = point[idx];
      Future f = get_future(dp);
      return f.get_void_result();
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    Realm::ZIndexSpace<DIM,T> PhysicalRegion::get_bounds(void) const
    //--------------------------------------------------------------------------
    {
      Realm::ZIndexSpace<DIM,T> result;
      get_bounds(&result, Internal::NT_TemplateHelper::encode_tag<DIM,T>());
      return result;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    PhysicalRegion::operator Realm::ZIndexSpace<DIM,T>(void) const
    //--------------------------------------------------------------------------
    {
      Realm::ZIndexSpace<DIM,T> result;
      get_bounds(&result, Internal::NT_TemplateHelper::encode_tag<DIM,T>());
      return result;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    PhysicalRegion::operator Realm::ZRect<DIM,T>(void) const
    //--------------------------------------------------------------------------
    {
      Realm::ZIndexSpace<DIM,T> result;
      get_bounds(&result, Internal::NT_TemplateHelper::encode_tag<DIM,T>());
#ifdef DEBUG_LEGION
      assert(result.dense());
#endif
      return result.bounds;
    }

    //--------------------------------------------------------------------------
    inline bool IndexIterator::has_next(void) const
    //--------------------------------------------------------------------------
    {
      return *iterator;
    }
    
    //--------------------------------------------------------------------------
    inline ptr_t IndexIterator::next(void)
    //--------------------------------------------------------------------------
    {
      ptr_t result = iterator->p[0];
      iterator->step();
      return result;
    }

    //--------------------------------------------------------------------------
    inline ptr_t IndexIterator::next_span(size_t& act_count, size_t req_count)
    //--------------------------------------------------------------------------
    {
      // This is slow for backwards compatability
      ptr_t result = iterator->p[0];
      iterator->step();
      act_count = 1;
      return result;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T> Runtime::create_index_space(Context ctx, 
                                                   Realm::ZRect<DIM,T> bounds)
    //--------------------------------------------------------------------------
    {
      // Make a Realm index space
      Realm::ZIndexSpace<DIM,T> realm_is(bounds);
      return IndexSpaceT<DIM,T>(create_index_space_internal(ctx, &realm_is,
                Internal::NT_TemplateHelper::template encode_tag<DIM,T>()));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T> Runtime::union_index_spaces(Context ctx,
                                 const std::vector<IndexSpaceT<DIM,T> > &spaces)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> handles(spaces.size());
      for (unsigned idx = 0; idx < spaces.size(); idx++)
        handles[idx] = spaces[idx];
      return IndexSpaceT<DIM,T>(union_index_spaces(ctx, handles));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T> Runtime::intersect_index_spaces(Context ctx,
                                 const std::vector<IndexSpaceT<DIM,T> > &spaces)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> handles(spaces.size());
      for (unsigned idx = 0; idx < spaces.size(); idx++)
        handles[idx] = spaces[idx];
      return IndexSpaceT<DIM,T>(intersect_index_spaces(ctx, handles));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T> Runtime::subtract_index_spaces(Context ctx,
                              IndexSpaceT<DIM,T> left, IndexSpaceT<DIM,T> right)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<DIM,T>(subtract_index_spaces(ctx, 
                                        IndexSpace(left), IndexSpace(right)));
    }

    //--------------------------------------------------------------------------
    template<typename T>
    IndexPartition Runtime::create_index_partition(Context ctx,
      IndexSpace parent, const T& mapping, Color part_color /*= AUTO_GENERATE*/)
    //--------------------------------------------------------------------------
    {
      LegionRuntime::Arrays::Rect<T::IDIM> parent_rect = 
        get_index_space_domain(ctx, parent).get_rect<T::IDIM>();
      LegionRuntime::Arrays::Rect<T::ODIM> color_space = 
        mapping.image_convex(parent_rect);
      DomainPointColoring c;
      for (typename T::PointInOutputRectIterator pir(color_space); 
          pir; pir++) 
      {
        LegionRuntime::Arrays::Rect<T::IDIM> preimage = mapping.preimage(pir.p);
#ifdef DEBUG_LEGION
        assert(mapping.preimage_is_dense(pir.p));
#endif
        c[DomainPoint::from_point<T::IDIM>(pir.p)] =
          Domain::from_rect<T::IDIM>(preimage.intersection(parent_rect));
      }
      IndexPartition result = create_index_partition(ctx, parent, 
              Domain::from_rect<T::ODIM>(color_space), c, 
              DISJOINT_KIND, part_color);
#ifdef DEBUG_LEGION
      // We don't actually know if we're supposed to check disjointness
      // so if we're in debug mode then just do it.
      {
        std::set<DomainPoint> current_colors;  
        for (DomainPointColoring::const_iterator it1 = c.begin();
              it1 != c.end(); it1++)
        {
          current_colors.insert(it1->first);
          for (DomainPointColoring::const_iterator it2 = c.begin();
                it2 != c.end(); it2++)
          {
            if (current_colors.find(it2->first) != current_colors.end())
              continue;
            LegionRuntime::Arrays::Rect<T::IDIM> rect1 = 
              it1->second.get_rect<T::IDIM>();
            LegionRuntime::Arrays::Rect<T::IDIM> rect2 = 
              it2->second.get_rect<T::IDIM>();
            if (rect1.overlaps(rect2))
            {
              switch (it1->first.dim)
              {
                case 1:
                  fprintf(stderr, "ERROR: colors %d and %d of partition %d are "
                                  "not disjoint rectangles as they should be!",
                                   (int)(it1->first)[0],
                                   (int)(it2->first)[0], result.id);
                  break;
                case 2:
                  fprintf(stderr, "ERROR: colors (%d, %d) and (%d, %d) of "
                                  "partition %d are not disjoint rectangles "
                                  "as they should be!",
                                  (int)(it1->first)[0], (int)(it1->first)[1],
                                  (int)(it2->first)[0], (int)(it2->first)[1],
                                  result.id);
                  break;
                case 3:
                  fprintf(stderr, "ERROR: colors (%d, %d, %d) and (%d, %d, %d) "
                                  "of partition %d are not disjoint rectangles "
                                  "as they should be!",
                                  (int)(it1->first)[0], (int)(it1->first)[1],
                                  (int)(it1->first)[2], (int)(it2->first)[0],
                                  (int)(it2->first)[1], (int)(it2->first)[2],
                                  result.id);
                  break;
                default:
                  assert(false);
              }
              assert(false);
              exit(ERROR_DISJOINTNESS_TEST_FAILURE);
            }
          }
        }
      }
#endif
      return result;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_equal_partition(Context ctx,
                              IndexSpaceT<DIM,T> parent,
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              size_t granularity, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_equal_partition(ctx,
                                    IndexSpace(parent), IndexSpace(color_space),
                                    granularity, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_union(Context ctx,
                              IndexSpaceT<DIM,T> parent,
                              IndexPartitionT<DIM,T> handle1,
                              IndexPartitionT<DIM,T> handle2,
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_partition_by_union(ctx,
           IndexSpace(parent), IndexPartition(handle1),
           IndexPartition(handle2), IndexSpace(color_space), part_kind, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_intersection(
                              Context ctx,
                              IndexSpaceT<DIM,T> parent,
                              IndexPartitionT<DIM,T> handle1,
                              IndexPartitionT<DIM,T> handle2,
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_partition_by_intersection(ctx,
           IndexSpace(parent), IndexPartition(handle1),
           IndexPartition(handle2), IndexSpace(color_space), part_kind, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_difference(Context ctx,
                              IndexSpaceT<DIM,T> parent,
                              IndexPartitionT<DIM,T> handle1,
                              IndexPartitionT<DIM,T> handle2,
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_partition_by_difference(ctx,
           IndexSpace(parent), IndexPartition(handle1),
           IndexPartition(handle2), IndexSpace(color_space), part_kind, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    Color Runtime::create_cross_product_partitions(Context ctx,
                                      IndexPartitionT<DIM,T> handle1,
                                      IndexPartitionT<DIM,T> handle2,
                                      typename std::map<
                                        IndexSpaceT<DIM,T>,
                                        IndexPartitionT<DIM,T> > &handles,
                                      PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      std::map<IndexSpace,IndexPartition> untyped_handles;
      for (typename std::map<IndexSpaceT<DIM,T>,
                             IndexPartitionT<DIM,T> >::const_iterator it =
            handles.begin(); it != handles.end(); it++)
        untyped_handles[it->first] = IndexPartition::NO_PART;
      Color result = create_cross_product_partitions(ctx, handle1, handle2, 
                                        untyped_handles, part_kind, color);
      for (typename std::map<IndexSpaceT<DIM,T>,
                             IndexPartitionT<DIM,T> >::iterator it =
            handles.begin(); it != handles.end(); it++)
      {
        std::map<IndexSpace,IndexPartition>::const_iterator finder = 
          untyped_handles.find(it->first);
#ifdef DEBUG_LEGION
        assert(finder != untyped_handles.end());
#endif
        it->second = IndexPartitionT<DIM,T>(finder->second);
      }
      return result;
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2>
    void Runtime::create_association(Context ctx,
                                     LogicalRegionT<DIM1,T1> domain,
                                     LogicalRegionT<DIM1,T1> domain_parent,
                                     FieldID domain_fid,
                                     IndexSpaceT<DIM2,T2> range,
                                     MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      create_association(ctx, LogicalRegion(domain), 
          LogicalRegion(domain_parent), domain_fid, IndexSpace(range), id, tag);
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2>
    void Runtime::create_bidirectional_association(Context ctx,
                                      LogicalRegionT<DIM1,T1> domain,
                                      LogicalRegionT<DIM1,T1> domain_parent,
                                      FieldID domain_fid,
                                      LogicalRegionT<DIM2,T2> range,
                                      LogicalRegionT<DIM2,T2> range_parent,
                                      FieldID range_fid,
                                      MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      create_bidirectional_association(ctx, LogicalRegion(domain),
                                       LogicalRegion(domain_parent), domain_fid,
                                       LogicalRegion(range),
                                       LogicalRegion(range_parent), 
                                       range_fid, id, tag);
    }

    //--------------------------------------------------------------------------
    template<int DIM, int COLOR_DIM, typename T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_restriction(Context ctx,
                                      IndexSpaceT<DIM,T> parent,
                                      IndexSpaceT<COLOR_DIM,T> color_space,
                                      Realm::ZMatrix<DIM,COLOR_DIM,T> transform,
                                      Realm::ZRect<DIM,T> extent,
                                      PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_restricted_partition(ctx,
        parent, color_space, &transform, sizeof(transform), 
        &extent, sizeof(extent), part_kind, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_blockify(Context ctx,
                                      IndexSpaceT<DIM,T> parent,
                                      Realm::ZPoint<DIM,T> blocking_factor,
                                      Color color)
    //--------------------------------------------------------------------------
    {
      Realm::ZPoint<DIM,T> origin; 
      for (int i = 0; i < DIM; i++)
        origin[i] = 0;
      return create_partition_by_blockify<DIM,T>(ctx, parent, blocking_factor,
                                                 origin, color);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_blockify(Context ctx,
                                      IndexSpaceT<DIM,T> parent,
                                      Realm::ZPoint<DIM,T> blocking_factor,
                                      Realm::ZPoint<DIM,T> origin,
                                      Color color)
    //--------------------------------------------------------------------------
    {
      // Get the domain of the color space to partition
      const Realm::ZIndexSpace<DIM,T> parent_is = 
        get_index_space_domain(parent);
      const Realm::ZRect<DIM,T> &bounds = parent_is.bounds;
      if (bounds.empty())
        return IndexPartitionT<DIM,T>();
      // Compute the intended color space bounds
      Realm::ZPoint<DIM,T> colors;
      for (int i = 0; i < DIM; i++)
        colors[i] = (((bounds.hi[i] - bounds.lo[i]) + // -1 and +1 cancel out
            blocking_factor[i]) / blocking_factor[i]) - 1; 
      Realm::ZPoint<DIM,T> zeroes; 
      for (int i = 0; i < DIM; i++)
        zeroes[i] = 0;
      // Make the color space
      IndexSpaceT<DIM,T> color_space = create_index_space(ctx, 
                                    Realm::ZRect<DIM,T>(zeroes, colors));
      // Now make the transform matrix
      Realm::ZMatrix<DIM,DIM,T> transform;
      for (int i = 0; i < DIM; i++)
        for (int j = 0; j < DIM; j++)
          if (i == j)
            transform[i][j] = blocking_factor[i];
          else
            transform[i][j] = 0;
      // And the extent
      Realm::ZPoint<DIM,T> ones;
      for (int i = 0; i < DIM; i++)
        ones[i] = 1;
      const Realm::ZRect<DIM,T> extent(origin, origin + blocking_factor - ones);
      // Then do the create partition by restriction call
      return create_partition_by_restriction(ctx, parent, color_space,
                                             transform, extent,
                                             DISJOINT_KIND, color);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_partition_by_field(Context ctx,
                                    LogicalRegionT<DIM,T> handle,
                                    LogicalRegionT<DIM,T> parent,
                                    FieldID fid,
                                    IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                                    Color color, MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_partition_by_field(ctx,
            LogicalRegion(handle), LogicalRegion(parent), fid, 
            IndexSpace(color_space), color, id, tag));
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2,
             int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM2,T2> Runtime::create_partition_by_image(Context ctx,
                              IndexSpaceT<DIM2,T2> handle,
                              LogicalPartitionT<DIM1,T1> projection,
                              LogicalRegionT<DIM1,T1> parent,
                              FieldID fid, // type: ZPoint<DIM2,COORD_T2>
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color,
                              MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM2,T2>(create_partition_by_image(ctx,
        IndexSpace(handle), LogicalPartition(projection),
        LogicalRegion(parent), fid, IndexSpace(color_space), part_kind, 
        color, id, tag));
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2,
             int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM2,T2> Runtime::create_partition_by_image_range(
                              Context ctx,
                              IndexSpaceT<DIM2,T2> handle,
                              LogicalPartitionT<DIM1,T1> projection,
                              LogicalRegionT<DIM1,T1> parent,
                              FieldID fid, // type: ZPoint<DIM2,COORD_T2>
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color,
                              MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM2,T2>(create_partition_by_image_range(ctx,
        IndexSpace(handle), LogicalPartition(projection),
        LogicalRegion(parent), fid, IndexSpace(color_space), part_kind, 
        color, id, tag));
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2,
             int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM1,T1> Runtime::create_partition_by_preimage(Context ctx,
                              IndexPartitionT<DIM2,T2> projection,
                              LogicalRegionT<DIM1,T1> handle,
                              LogicalRegionT<DIM1,T1> parent,
                              FieldID fid, // type: ZPoint<DIM2,COORD_T2>
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color,
                              MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM1,T1>(create_partition_by_preimage(ctx, 
        IndexPartition(projection), LogicalRegion(handle),
        LogicalRegion(parent), fid, IndexSpace(color_space), part_kind, 
        color, id, tag));
    }

    //--------------------------------------------------------------------------
    template<int DIM1, typename T1, int DIM2, typename T2,
             int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM1,T1> Runtime::create_partition_by_preimage_range(
                              Context ctx,
                              IndexPartitionT<DIM2,T2> projection,
                              LogicalRegionT<DIM1,T1> handle,
                              LogicalRegionT<DIM1,T1> parent,
                              FieldID fid, // type: ZRect<DIM2,COORD_T2>
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color,
                              MapperID id, MappingTagID tag)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM1,T1>(create_partition_by_preimage_range(ctx,
        IndexPartition(projection), LogicalRegion(handle), 
        LogicalRegion(parent), fid, IndexSpace(color_space), part_kind, 
        color, id, tag));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexPartitionT<DIM,T> Runtime::create_pending_partition(Context ctx,
                              IndexSpaceT<DIM,T> parent,
                              IndexSpaceT<COLOR_DIM,COLOR_T> color_space,
                              PartitionKind part_kind, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(create_pending_partition(ctx,
            IndexSpace(parent), IndexSpace(color_space), part_kind, color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::create_index_space_union(Context ctx,
                                IndexPartitionT<DIM,T> parent,
                                Realm::ZPoint<COLOR_DIM,COLOR_T> color,
                                const typename std::vector<
                                  IndexSpaceT<DIM,T> > &handles)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> untyped_handles(handles.size());
      for (unsigned idx = 0; idx < handles.size(); idx++)
        untyped_handles[idx] = handles[idx];
      return IndexSpaceT<DIM,T>(create_index_space_union_internal(ctx, 
            IndexPartition(parent), &color, 
            Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>(),
            untyped_handles));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::create_index_space_union(Context ctx,
                                IndexPartitionT<DIM,T> parent,
                                Realm::ZPoint<COLOR_DIM,COLOR_T> color,
                                IndexPartitionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<DIM,T>(create_index_space_union_internal(ctx,
          IndexPartition(parent), &color, 
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>(),
          IndexPartition(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::create_index_space_intersection(Context ctx,
                                IndexPartitionT<DIM,T> parent,
                                Realm::ZPoint<COLOR_DIM,COLOR_T> color,
                                const typename std::vector<
                                  IndexSpaceT<DIM,T> > &handles)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> untyped_handles(handles.size());
      for (unsigned idx = 0; idx < handles.size(); idx++)
        untyped_handles[idx] = handles[idx];
      return IndexSpaceT<DIM,T>(create_index_space_intersection_internal(ctx,
            IndexPartition(parent), &color,
            Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>(), 
            untyped_handles));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::create_index_space_intersection(Context ctx,
                                IndexPartitionT<DIM,T> parent,
                                Realm::ZPoint<COLOR_DIM,COLOR_T> color,
                                IndexPartitionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<DIM,T>(create_index_space_intersection_internal(ctx,
          IndexPartition(parent), &color, 
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>(),
          IndexPartition(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::create_index_space_difference(Context ctx,
                                IndexPartitionT<DIM,T> parent,
                                Realm::ZPoint<COLOR_DIM,COLOR_T> color,
                                IndexSpaceT<DIM,T> initial,
                                const typename std::vector<
                                  IndexSpaceT<DIM,T> > &handles)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> untyped_handles(handles.size());
      for (unsigned idx = 0; idx < handles.size(); idx++)
        untyped_handles[idx] = handles[idx];
      return IndexSpaceT<DIM,T>(create_index_space_difference_internal(ctx,
            IndexPartition(parent), &color,
            Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>(), 
            IndexSpace(initial), untyped_handles));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T> Runtime::get_index_partition(
                                         IndexSpaceT<DIM,T> parent, Color color)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(
                          get_index_partition(IndexSpace(parent), color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    bool Runtime::has_index_partition(IndexSpaceT<DIM,T> parent, Color color)
    //--------------------------------------------------------------------------
    {
      return has_index_partition(IndexSpace(parent), color);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<DIM,T> Runtime::get_index_subspace(IndexPartitionT<DIM,T> p,
                                         Realm::ZPoint<COLOR_DIM,COLOR_T> color)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<DIM,T>(get_index_subspace_internal(IndexPartition(p), 
        &color, Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>()));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    bool Runtime::has_index_subspace(IndexPartitionT<DIM,T> p, 
                                     Realm::ZPoint<COLOR_DIM,COLOR_T> color)
    //--------------------------------------------------------------------------
    {
      return has_index_subspace_internal(IndexPartition(p), &color,
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    Realm::ZIndexSpace<DIM,T> Runtime::get_index_space_domain(IndexSpace handle)
    //--------------------------------------------------------------------------
    {
      Realm::ZIndexSpace<DIM,T> realm_is;
      get_index_space_domain_internal(handle, &realm_is, 
          Internal::NT_TemplateHelper::encode_tag<DIM,T>());
      return realm_is;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    Realm::ZIndexSpace<COLOR_DIM,COLOR_T> 
              Runtime::get_index_partition_color_space(IndexPartitionT<DIM,T> p)
    //--------------------------------------------------------------------------
    {
      Realm::ZIndexSpace<COLOR_DIM, COLOR_T> realm_is;
      get_index_partition_color_space_internal(p, &realm_is, 
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>());
      return realm_is;
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    IndexSpaceT<COLOR_DIM,COLOR_T> 
         Runtime::get_index_partition_color_space_name(IndexPartitionT<DIM,T> p)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<COLOR_DIM,COLOR_T>(
                              get_index_partition_color_space_name(p));
    }

    //--------------------------------------------------------------------------
    template<unsigned DIM>
    IndexSpace Runtime::get_index_subspace(Context ctx, 
                IndexPartition p, LegionRuntime::Arrays::Point<DIM> color_point)
    //--------------------------------------------------------------------------
    {
      DomainPoint dom_point = DomainPoint::from_point<DIM>(color_point);
      return get_index_subspace(ctx, p, dom_point);
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    Realm::ZPoint<COLOR_DIM,COLOR_T> Runtime::get_index_space_color(
                                                      IndexSpaceT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      Realm::ZPoint<COLOR_DIM,COLOR_T> point;
      return get_index_space_color_internal(IndexSpace(handle), &point,
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexSpaceT<DIM,T> Runtime::get_parent_index_space(
                                                  IndexPartitionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return IndexSpaceT<DIM,T>(get_parent_index_space(IndexPartiiton(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    IndexPartitionT<DIM,T> Runtime::get_parent_index_partition(
                                                      IndexSpaceT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return IndexPartitionT<DIM,T>(get_parent_index_partition(
                                              IndexSpace(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    bool Runtime::safe_cast(Context ctx, Realm::ZPoint<DIM,T> point, 
                            LogicalRegionT<DIM,T> region)
    //--------------------------------------------------------------------------
    {
      return safe_cast_internal(ctx, LogicalRegion(region), &point,
          Internal::NT_TemplateHelper::encode_tag<DIM,T>());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T> Runtime::create_logical_region(Context ctx,
                                    IndexSpaceT<DIM,T> index, FieldSpace fields)
    //--------------------------------------------------------------------------
    {
      return LogicalRegionT<DIM,T>(create_logical_region(ctx, 
                                  IndexSpace(index), fields));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T> Runtime::get_logical_partition(
                    LogicalRegionT<DIM,T> parent, IndexPartitionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return LogicalPartitionT<DIM,T>(get_logical_partition(
                LogicalRegion(parent), IndexPartition(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T> Runtime::get_logical_partition_by_color(
                                      LogicalRegionT<DIM,T> parent, Color color)
    //--------------------------------------------------------------------------
    {
      return LogicalPartitionT<DIM,T>(get_logical_partition_by_color(
                                        LogicalRegion(parent), color));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T> Runtime::get_logical_partition_by_tree(
              IndexPartitionT<DIM,T> handle, FieldSpace space, RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      return LogicalPartitionT<DIM,T>(get_logical_partition_by_tree(
                                  IndexPartition(handle), space, tid));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T> Runtime::get_logical_subregion(
                     LogicalPartitionT<DIM,T> parent, IndexSpaceT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return LogicalRegionT<DIM,T>(get_logical_subregion(
                LogicalPartition(parent), IndexSpace(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    LogicalRegionT<DIM,T> Runtime::get_logical_subregion_by_color(
        LogicalPartitionT<DIM,T> parent, Realm::ZPoint<COLOR_DIM,COLOR_T> color)
    //--------------------------------------------------------------------------
    {
      return LogicalRegionT<DIM,T>(get_logical_subregion_by_color_internal(
            LogicalPartition(parent), &color,
            Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>()));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    bool Runtime::has_logical_subregion_by_color(
        LogicalPartitionT<DIM,T> parent, Realm::ZPoint<COLOR_DIM,COLOR_T> color)
    //--------------------------------------------------------------------------
    {
      return has_logical_subregion_by_color_internal(
          LogicalPartition(parent), &color,
          Internal::NT_TemplateHelper::encode_tag<COLOR_DIM,COLOR_T>());
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T> Runtime::get_logical_subregion_by_tree(
                  IndexSpaceT<DIM,T> handle, FieldSpace space, RegionTreeID tid)
    //--------------------------------------------------------------------------
    {
      return LogicalRegionT<DIM,T>(get_logical_subregion_by_tree(
                                    IndexSpace(handle), space, tid));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T, int COLOR_DIM, typename COLOR_T>
    Realm::ZPoint<COLOR_DIM,COLOR_T> Runtime::get_logical_region_color_point(
                                                   LogicalRegionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return get_logical_region_color_point(LogicalRegion(handle));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalRegionT<DIM,T> Runtime::get_parent_logical_region(
                                                LogicalPartitionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return LogicalRegionT<DIM,T>(get_parent_logical_region(
                                    LogicalPartition(handle)));
    }

    //--------------------------------------------------------------------------
    template<int DIM, typename T>
    LogicalPartitionT<DIM,T> Runtime::get_parent_logical_partition(
                                                   LogicalRegionT<DIM,T> handle)
    //--------------------------------------------------------------------------
    {
      return LogicalPartitionT<DIM,T>(get_parent_logical_partition(
                                            LogicalRegion(handle)));
    }

    //--------------------------------------------------------------------------
    template<typename T>
    void Runtime::fill_field(Context ctx, LogicalRegion handle,
                                      LogicalRegion parent, FieldID fid,
                                      const T &value, Predicate pred)
    //--------------------------------------------------------------------------
    {
      fill_field(ctx, handle, parent, fid, &value, sizeof(T), pred);
    }

    //--------------------------------------------------------------------------
    template<typename T>
    void Runtime::fill_fields(Context ctx, LogicalRegion handle,
                                       LogicalRegion parent, 
                                       const std::set<FieldID> &fields,
                                       const T &value, Predicate pred)
    //--------------------------------------------------------------------------
    {
      fill_fields(ctx, handle, parent, fields, &value, sizeof(T), pred);
    }

    //--------------------------------------------------------------------------
    template<typename T>
    T* Runtime::get_local_task_variable(Context ctx, LocalVariableID id)
    //--------------------------------------------------------------------------
    {
      return static_cast<T*>(get_local_task_variable_untyped(ctx, id));
    }

    //--------------------------------------------------------------------------
    template<typename T>
    void Runtime::set_local_task_variable(Context ctx, LocalVariableID id,
                                      const T* value, void (*destructor)(void*))
    //--------------------------------------------------------------------------
    {
      set_local_task_variable_untyped(ctx, id, value, destructor);
    }

    //--------------------------------------------------------------------------
    template<typename REDOP>
    /*static*/ void Runtime::register_reduction_op(ReductionOpID redop_id)
    //--------------------------------------------------------------------------
    {
      if (redop_id == 0)
      {
        fprintf(stderr,"ERROR: ReductionOpID zero is reserved.\n");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_RESERVED_REDOP_ID);
      }
      ReductionOpTable &red_table = Runtime::get_reduction_table(); 
      // Check to make sure we're not overwriting a prior reduction op 
      if (red_table.find(redop_id) != red_table.end())
      {
        fprintf(stderr,"ERROR: ReductionOpID %d has already been used " 
                       "in the reduction table\n",redop_id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_DUPLICATE_REDOP_ID);
      }
      red_table[redop_id] = 
        Realm::ReductionOpUntyped::create_reduction_op<REDOP>(); 
      // We also have to check to see if there are explicit serialization
      // and deserialization methods on the RHS type for doing fold reductions
      SerdezRedopTable &serdez_red_table = Runtime::get_serdez_redop_table();
      LegionSerialization::register_reduction<REDOP>(serdez_red_table,redop_id);
    }

    //--------------------------------------------------------------------------
    template<typename SERDEZ>
    /*static*/ void Runtime::register_custom_serdez_op(CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
      if (serdez_id == 0)
      {
        fprintf(stderr,"ERROR: Custom Serdez ID zero is reserved.\n");
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_RESERVED_SERDEZ_ID);
      }
      SerdezOpTable &serdez_table = Runtime::get_serdez_table();
      // Check to make sure we're not overwriting a prior serdez op
      if (serdez_table.find(serdez_id) != serdez_table.end())
      {
        fprintf(stderr,"ERROR: CustomSerdezID %d has already been used "
                       "in the serdez operation table\n", serdez_id);
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_DUPLICATE_SERDEZ_ID);
      }
      serdez_table[serdez_id] =
        Realm::CustomSerdezUntyped::create_custom_serdez<SERDEZ>();
    }

    namespace Internal {
      // Wrapper class for old projection functions
      template<RegionProjectionFnptr FNPTR>
      class RegionProjectionWrapper : public ProjectionFunctor {
      public:
        RegionProjectionWrapper(void) 
          : ProjectionFunctor() { }
        virtual ~RegionProjectionWrapper(void) { }
      public:
        virtual LogicalRegion project(Context ctx, Task *task,
                                      unsigned index,
                                      LogicalRegion upper_bound,
                                      const DomainPoint &point)
        {
          return (*FNPTR)(upper_bound, point, runtime); 
        }
        virtual LogicalRegion project(Context ctx, Task *task,
                                      unsigned index,
                                      LogicalPartition upper_bound,
                                      const DomainPoint &point)
        {
          assert(false);
          return LogicalRegion::NO_REGION;
        }
        virtual bool is_exclusive(void) const { return false; }
      };
    };

    //--------------------------------------------------------------------------
    template<LogicalRegion (*PROJ_PTR)(LogicalRegion, const DomainPoint&,
                                       Runtime*)>
    /*static*/ ProjectionID Runtime::register_region_function(
                                                            ProjectionID handle)
    //--------------------------------------------------------------------------
    {
      Runtime::preregister_projection_functor(handle,
          new Internal::RegionProjectionWrapper<PROJ_PTR>());
      return handle;
    }

    namespace Internal {
      // Wrapper class for old projection functions
      template<PartitionProjectionFnptr FNPTR>
      class PartitionProjectionWrapper : public ProjectionFunctor {
      public:
        PartitionProjectionWrapper(void)
          : ProjectionFunctor() { }
        virtual ~PartitionProjectionWrapper(void) { }
      public:
        virtual LogicalRegion project(Context ctx, Task *task,
                                      unsigned index,
                                      LogicalRegion upper_bound,
                                      const DomainPoint &point)
        {
          assert(false);
          return LogicalRegion::NO_REGION;
        }
        virtual LogicalRegion project(Context ctx, Task *task,
                                      unsigned index,
                                      LogicalPartition upper_bound,
                                      const DomainPoint &point)
        {
          return (*FNPTR)(upper_bound, point, runtime);
        }
        virtual bool is_exclusive(void) const { return false; }
      };
    };

    //--------------------------------------------------------------------------
    template<LogicalRegion (*PROJ_PTR)(LogicalPartition, const DomainPoint&,
                                       Runtime*)>
    /*static*/ ProjectionID Runtime::register_partition_function(
                                                    ProjectionID handle)
    //--------------------------------------------------------------------------
    {
      Runtime::preregister_projection_functor(handle,
          new Internal::PartitionProjectionWrapper<PROJ_PTR>());
      return handle;
    }

    //--------------------------------------------------------------------------
    // Wrapper functions for high-level tasks
    //--------------------------------------------------------------------------

    /**
     * \class LegionTaskWrapper
     * This is a helper class that has static template methods for 
     * wrapping Legion application tasks.  For all tasks we can make
     * wrappers both for normal execution and also for inline execution.
     */
    class LegionTaskWrapper {
    public: 
      // Non-void return type for new legion task types
      template<typename T,
        T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                      Context, Runtime*)>
      static void legion_task_wrapper(const void*, size_t, 
                                      const void*, size_t, Processor);
      template<typename T, typename UDT,
        T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                      Context, Runtime*, const UDT&)>
      static void legion_udt_task_wrapper(const void*, size_t, 
                                          const void*, size_t, Processor);
    public:
      // Void return type for new legion task types
      template<
        void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                         Context, Runtime*)>
      static void legion_task_wrapper(const void*, size_t, 
                                      const void*, size_t, Processor);
      template<typename UDT,
        void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                         Context, Runtime*, const UDT&)>
      static void legion_udt_task_wrapper(const void*, size_t, 
                                          const void*, size_t, Processor);

    public:
      // Do-it-yourself pre/post-ambles for code generators
      static void legion_task_preamble(const void *data,
				       size_t datalen,
				       Processor p,
				       const Task *& task,
				       const std::vector<PhysicalRegion> *& regionsptr,
				       Context& ctx,
				       Runtime *& runtime);
      static void legion_task_postamble(Runtime *runtime, Context ctx,
					const void *retvalptr = NULL,
					size_t retvalsize = 0);
    };
    
    //--------------------------------------------------------------------------
    template<typename T,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*)>
    void LegionTaskWrapper::legion_task_wrapper(const void *args, 
                                                size_t arglen, 
                                                const void *userdata,
                                                size_t userlen,
                                                Processor p)
    //--------------------------------------------------------------------------
    {
      // Assert that we are returning Futures or FutureMaps
      LEGION_STATIC_ASSERT((LegionTypeInequality<T,Future>::value));
      LEGION_STATIC_ASSERT((LegionTypeInequality<T,FutureMap>::value));
      // Assert that the return type size is within the required size
      LEGION_STATIC_ASSERT(sizeof(T) <= MAX_RETURN_SIZE);
      // Get the high level runtime
      Runtime *runtime = Runtime::get_runtime(p);
      // Read the context out of the buffer
#ifdef DEBUG_LEGION
      assert(arglen == sizeof(InternalContext));
#endif
      InternalContext ctx = *((const InternalContext*)args);

      const std::vector<PhysicalRegion> &regions = ctx->begin_task();

      // Invoke the task with the given context
      T return_value = 
        (*TASK_PTR)(ctx->get_task(), regions, ctx->as_context(), runtime);

      // Send the return value back
      LegionSerialization::end_task<T>(runtime, ctx, &return_value);
    }

    //--------------------------------------------------------------------------
    template<
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*)>
    void LegionTaskWrapper::legion_task_wrapper(const void *args, 
                                                size_t arglen, 
                                                const void *userdata,
                                                size_t userlen,
                                                Processor p)
    //--------------------------------------------------------------------------
    {
      // Get the high level runtime
      Runtime *runtime = Runtime::get_runtime(p);

      // Read the context out of the buffer
#ifdef DEBUG_LEGION
      assert(arglen == sizeof(InternalContext));
#endif
      InternalContext ctx = *((const InternalContext*)args);

      const std::vector<PhysicalRegion> &regions = ctx->begin_task(); 

      (*TASK_PTR)(ctx->get_task(), regions, ctx->as_context(), runtime);

      // Send an empty return value back
      ctx->end_task(NULL, 0, false);
    }

    //--------------------------------------------------------------------------
    template<typename T, typename UDT,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*, const UDT&)>
    void LegionTaskWrapper::legion_udt_task_wrapper(const void *args,
                                                    size_t arglen, 
                                                    const void *userdata,
                                                    size_t userlen,
                                                    Processor p)
    //--------------------------------------------------------------------------
    {
      // Assert that we are returning Futures or FutureMaps
      LEGION_STATIC_ASSERT((LegionTypeInequality<T,Future>::value));
      LEGION_STATIC_ASSERT((LegionTypeInequality<T,FutureMap>::value));
      // Assert that the return type size is within the required size
      LEGION_STATIC_ASSERT(sizeof(T) <= MAX_RETURN_SIZE);
      // Get the high level runtime
      Runtime *runtime = Runtime::get_runtime(p);

      // Read the context out of the buffer
#ifdef DEBUG_LEGION
      assert(arglen == sizeof(InternalContext));
#endif
      InternalContext ctx = *((const InternalContext*)args);

      const UDT *user_data = reinterpret_cast<const UDT*>(userdata);

      const std::vector<PhysicalRegion> &regions = ctx->begin_task(); 

      // Invoke the task with the given context
      T return_value = (*TASK_PTR)(ctx->get_task(), regions, 
                                   ctx->as_context(), runtime, *user_data);

      // Send the return value back
      LegionSerialization::end_task<T>(runtime, ctx, &return_value);
    }

    //--------------------------------------------------------------------------
    template<typename UDT,
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*, const UDT&)>
    void LegionTaskWrapper::legion_udt_task_wrapper(const void *args,
                                                    size_t arglen, 
                                                    const void *userdata,
                                                    size_t userlen,
                                                    Processor p)
    //--------------------------------------------------------------------------
    {
      // Get the high level runtime
      Runtime *runtime = Runtime::get_runtime(p);

      // Read the context out of the buffer
#ifdef DEBUG_LEGION
      assert(arglen == sizeof(InternalContext));
#endif
      InternalContext ctx = *((const InternalContext*)args);

      const UDT *user_data = reinterpret_cast<const UDT*>(userdata);

      const std::vector<PhysicalRegion> &regions = ctx->begin_task(); 

      (*TASK_PTR)(ctx->get_task(), regions, 
                  ctx->as_context(), runtime, *user_data);

      // Send an empty return value back
      ctx->end_task(NULL, 0, false);
    }

    //--------------------------------------------------------------------------
    template<typename T,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*)>
    VariantID Runtime::register_task_variant(
                                          const TaskVariantRegistrar &registrar)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
           LegionTaskWrapper::legion_task_wrapper<T,TASK_PTR>);
      return register_variant(registrar, true, NULL/*UDT*/, 0/*sizeof(UDT)*/,
                              realm_desc);
    }

    //--------------------------------------------------------------------------
    template<typename T, typename UDT,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*, const UDT&)>
    VariantID Runtime::register_task_variant(
                    const TaskVariantRegistrar &registrar, const UDT &user_data)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
           LegionTaskWrapper::legion_udt_task_wrapper<T,UDT,TASK_PTR>);
      return register_variant(registrar, true, &user_data, sizeof(UDT),
                              realm_desc);
    }

    //--------------------------------------------------------------------------
    template<
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*)>
    VariantID Runtime::register_task_variant(
                                          const TaskVariantRegistrar &registrar)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_task_wrapper<TASK_PTR>);
      return register_variant(registrar, false, NULL/*UDT*/, 0/*sizeof(UDT)*/,
                              realm_desc);
    }

    //--------------------------------------------------------------------------
    template<typename UDT,
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*, const UDT&)>
    VariantID Runtime::register_task_variant(
                    const TaskVariantRegistrar &registrar, const UDT &user_data)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_udt_task_wrapper<UDT,TASK_PTR>);
      return register_variant(registrar, false, &user_data, sizeof(UDT),
                              realm_desc);
    }

    //--------------------------------------------------------------------------
    template<typename T,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*)>
    /*static*/ VariantID Runtime::preregister_task_variant(
        const TaskVariantRegistrar &registrar, 
        const char *task_name /*= NULL*/, VariantID vid /*=AUTO_GENERATE_ID*/)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
          LegionTaskWrapper::legion_task_wrapper<T,TASK_PTR>);
      return preregister_variant(registrar, NULL/*UDT*/, 0/*sizeof(UDT)*/,
                                 realm_desc, true/*ret*/, task_name, vid);
    }

    //--------------------------------------------------------------------------
    template<typename T, typename UDT,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*, const UDT&)>
    /*static*/ VariantID Runtime::preregister_task_variant(
                    const TaskVariantRegistrar &registrar, 
                    const UDT &user_data, const char *task_name /*= NULL*/,
                    VariantID vid /*=AUTO_GENERATE_ID*/)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
          LegionTaskWrapper::legion_udt_task_wrapper<T,UDT,TASK_PTR>);
      return preregister_variant(registrar, &user_data, sizeof(UDT),
                               realm_desc, true/*ret*/, task_name, vid);
    }

    //--------------------------------------------------------------------------
    template<
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*)>
    /*static*/ VariantID Runtime::preregister_task_variant(
        const TaskVariantRegistrar &registrar, const char *task_name /*= NULL*/,
        const VariantID vid /*=AUTO_GENERATE_ID*/)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_task_wrapper<TASK_PTR>);
      return preregister_variant(registrar, NULL/*UDT*/,0/*sizeof(UDT)*/,
                             realm_desc, false/*ret*/, task_name, vid);
    }

    //--------------------------------------------------------------------------
    template<typename UDT,
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*, const UDT&)>
    /*static*/ VariantID Runtime::preregister_task_variant(
                    const TaskVariantRegistrar &registrar, 
                    const UDT &user_data, const char *task_name /*= NULL*/,
                    VariantID vid /*=AUTO_GENERATE_ID*/)
    //--------------------------------------------------------------------------
    {
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_udt_task_wrapper<UDT,TASK_PTR>);
      return preregister_variant(registrar, &user_data, sizeof(UDT),
                             realm_desc, false/*ret*/, task_name, vid);
    }

    //--------------------------------------------------------------------------
    template<typename T,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*)>
    /*static*/ TaskID Runtime::register_legion_task(TaskID id,
                                                    Processor::Kind proc_kind,
                                                    bool single, bool index,
                                                    VariantID vid,
                                                    TaskConfigOptions options,
                                                    const char *task_name)
    //--------------------------------------------------------------------------
    {
      bool check_task_id = true;
      if (id == AUTO_GENERATE_ID)
      {
        id = generate_static_task_id();
        check_task_id = false;
      }
      TaskVariantRegistrar registrar(id, task_name);
      registrar.set_leaf(options.leaf);
      registrar.set_inner(options.inner);
      registrar.set_idempotent(options.idempotent);
      registrar.add_constraint(ProcessorConstraint(proc_kind));
      CodeDescriptor *realm_desc = new CodeDescriptor(
          LegionTaskWrapper::legion_task_wrapper<T,TASK_PTR>);
      preregister_variant(registrar, NULL/*UDT*/, 0/*sizeof(UDT)*/,
                        realm_desc, true/*ret*/, task_name, vid, check_task_id);
      return id;
    }

    //--------------------------------------------------------------------------
    template<
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*)>
    /*static*/ TaskID Runtime::register_legion_task(TaskID id,
                                                    Processor::Kind proc_kind,
                                                    bool single, bool index,
                                                    VariantID vid,
                                                    TaskConfigOptions options,
                                                    const char *task_name)
    //--------------------------------------------------------------------------
    {
      bool check_task_id = true;
      if (id == AUTO_GENERATE_ID)
      {
        id = generate_static_task_id();
        check_task_id = false;
      }
      TaskVariantRegistrar registrar(id, task_name);
      registrar.set_leaf(options.leaf);
      registrar.set_inner(options.inner);
      registrar.set_idempotent(options.idempotent);
      registrar.add_constraint(ProcessorConstraint(proc_kind));
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_task_wrapper<TASK_PTR>);
      preregister_variant(registrar, NULL/*UDT*/, 0/*sizeof(UDT)*/,
                      realm_desc, false/*ret*/, task_name, vid, check_task_id);
      return id;
    }

    //--------------------------------------------------------------------------
    template<typename T, typename UDT,
      T (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                    Context, Runtime*, const UDT&)>
    /*static*/ TaskID Runtime::register_legion_task(TaskID id,
                                                    Processor::Kind proc_kind,
                                                    bool single, bool index,
                                                    const UDT &user_data,
                                                    VariantID vid,
                                                    TaskConfigOptions options,
                                                    const char *task_name)
    //--------------------------------------------------------------------------
    {
      bool check_task_id = true;
      if (id == AUTO_GENERATE_ID)
      {
        id = generate_static_task_id();
        check_task_id = false;
      }
      TaskVariantRegistrar registrar(id, task_name);
      registrar.set_leaf(options.leaf);
      registrar.set_inner(options.inner);
      registrar.set_idempotent(options.idempotent);
      registrar.add_constraint(ProcessorConstraint(proc_kind));
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_udt_task_wrapper<T,UDT,TASK_PTR>);
      preregister_variant(registrar, &user_data, sizeof(UDT),
                        realm_desc, true/*ret*/, task_name, vid, check_task_id);
      return id;
    }

    //--------------------------------------------------------------------------
    template<typename UDT,
      void (*TASK_PTR)(const Task*, const std::vector<PhysicalRegion>&,
                       Context, Runtime*, const UDT&)>
    /*static*/ TaskID Runtime::register_legion_task(TaskID id,
                                                    Processor::Kind proc_kind,
                                                    bool single, bool index,
                                                    const UDT &user_data,
                                                    VariantID vid,
                                                    TaskConfigOptions options,
                                                    const char *task_name)
    //--------------------------------------------------------------------------
    {
      bool check_task_id = true;
      if (id == AUTO_GENERATE_ID)
      {
        id = generate_static_task_id();
        check_task_id = false;
      }
      TaskVariantRegistrar registrar(id, task_name);
      registrar.set_leaf(options.leaf);
      registrar.set_inner(options.inner);
      registrar.set_idempotent(options.idempotent);
      registrar.add_constraint(ProcessorConstraint(proc_kind));
      CodeDescriptor *realm_desc = new CodeDescriptor(
            LegionTaskWrapper::legion_udt_task_wrapper<UDT,TASK_PTR>);
      preregister_variant(registrar, &user_data, sizeof(UDT),
                      realm_desc, false/*ret*/, task_name, vid, check_task_id);
      return id;
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator~(PrivilegeMode p)
    //--------------------------------------------------------------------------
    {
      return static_cast<PrivilegeMode>(~unsigned(p));
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator|(PrivilegeMode left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<PrivilegeMode>(unsigned(left) | unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator&(PrivilegeMode left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<PrivilegeMode>(unsigned(left) & unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator^(PrivilegeMode left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<PrivilegeMode>(unsigned(left) ^ unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator|=(PrivilegeMode &left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l |= r;
      return left = static_cast<PrivilegeMode>(l);
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator&=(PrivilegeMode &left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l &= r;
      return left = static_cast<PrivilegeMode>(l);
    }

    //--------------------------------------------------------------------------
    inline PrivilegeMode operator^=(PrivilegeMode &left, PrivilegeMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l ^= r;
      return left = static_cast<PrivilegeMode>(l);
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator~(AllocateMode a)
    //--------------------------------------------------------------------------
    {
      return static_cast<AllocateMode>(~unsigned(a));
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator|(AllocateMode left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<AllocateMode>(unsigned(left) | unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator&(AllocateMode left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<AllocateMode>(unsigned(left) & unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator^(AllocateMode left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      return static_cast<AllocateMode>(unsigned(left) ^ unsigned(right));
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator|=(AllocateMode &left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l |= r;
      return left = static_cast<AllocateMode>(l);
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator&=(AllocateMode &left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l &= r;
      return left = static_cast<AllocateMode>(l);
    }

    //--------------------------------------------------------------------------
    inline AllocateMode operator^=(AllocateMode &left, AllocateMode right)
    //--------------------------------------------------------------------------
    {
      unsigned l = static_cast<unsigned>(left);
      unsigned r = static_cast<unsigned>(right);
      l ^= r;
      return left = static_cast<AllocateMode>(l);
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os, const LogicalRegion& lr)
    //--------------------------------------------------------------------------
    {
      os << "LogicalRegion(" << lr.tree_id << "," 
         << lr.index_space << "," << lr.field_space << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os,const LogicalPartition& lp)
    //--------------------------------------------------------------------------
    {
      os << "LogicalPartition(" << lp.tree_id << "," 
         << lp.index_partition << "," << lp.field_space << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os, const IndexSpace& is)
    //--------------------------------------------------------------------------
    {
      os << "IndexSpace(" << is.id << "," << is.tid << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os, const IndexPartition& ip)
    //--------------------------------------------------------------------------
    {
      os << "IndexPartition(" << ip.id << "," << ip.tid << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os, const FieldSpace& fs)
    //--------------------------------------------------------------------------
    {
      os << "FieldSpace(" << fs.id << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    inline std::ostream& operator<<(std::ostream& os, const PhaseBarrier& pb)
    //--------------------------------------------------------------------------
    {
      os << "PhaseBarrier(" << pb.phase_barrier << ")";
      return os;
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline size_t Unserializable<T>::legion_buffer_size(void)
    //--------------------------------------------------------------------------
    {
      const std::type_info &info = typeid(T);
      fprintf(stderr,"ERROR: Illegal attempt to serialize Legion type %s. "
          "Objects of type %s are not allowed to be passed by value into or "
          "out of tasks.\n", info.name(), info.name());
      assert(false);
      return 0;
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline size_t Unserializable<T>::legion_serialize(void *buffer)
    //--------------------------------------------------------------------------
    {
      const std::type_info &info = typeid(T);
      fprintf(stderr,"ERROR: Illegal attempt to serialize Legion type %s. "
          "Objects of type %s are not allowed to be passed by value into or "
          "out of tasks.\n", info.name(), info.name());
      assert(false);
      return 0;
    }

    //--------------------------------------------------------------------------
    template<typename T>
    inline size_t Unserializable<T>::legion_deserialize(const void *buffer)
    //--------------------------------------------------------------------------
    {
      const std::type_info &info = typeid(T);
      fprintf(stderr,"ERROR: Illegal attempt to deserialize Legion type %s. "
          "Objects of type %s are not allowed to be passed by value into or "
          "out of tasks.\n", info.name(), info.name());
      assert(false);
      return 0;
    }

}; // namespace Legion

// This is for backwards compatibility with the old namespace scheme
namespace LegionRuntime {
  namespace HighLevel {
    using namespace LegionRuntime::Arrays;

    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexSpace IndexSpace;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexPartition IndexPartition;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::FieldSpace FieldSpace;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LogicalRegion LogicalRegion;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LogicalPartition LogicalPartition;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexAllocator IndexAllocator;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::FieldAllocator FieldAllocator;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskArgument TaskArgument;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ArgumentMap ArgumentMap;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Predicate Predicate;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Lock Lock;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LockRequest LockRequest;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Grant Grant;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::PhaseBarrier PhaseBarrier;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::DynamicCollective DynamicCollective;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::RegionRequirement RegionRequirement;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexSpaceRequirement IndexSpaceRequirement;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::FieldSpaceRequirement FieldSpaceRequirement;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Future Future;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::FutureMap FutureMap;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskLauncher TaskLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexLauncher IndexLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::InlineLauncher InlineLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::CopyLauncher CopyLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::PhysicalRegion PhysicalRegion;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::IndexIterator IndexIterator;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::AcquireLauncher AcquireLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ReleaseLauncher ReleaseLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskVariantRegistrar TaskVariantRegistrar;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::MustEpochLauncher MustEpochLauncher;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::MPILegionHandshake MPILegionHandshake;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Mappable Mappable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Task Task;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Copy Copy;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::InlineMapping Inline;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Acquire Acquire;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Release Release;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Mapping::Mapper Mapper;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::InputArgs InputArgs;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskConfigOptions TaskConfigOptions;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ProjectionFunctor ProjectionFunctor;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Runtime Runtime;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Runtime HighLevelRuntime; // for backwards compatibility
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ColoringSerializer ColoringSerializer;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::DomainColoringSerializer DomainColoringSerializer;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Serializer Serializer;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Deserializer Deserializer;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskResult TaskResult;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::CObjectWrapper CObjectWrapper;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ImmovableAutoLock AutoLock;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ISAConstraint ISAConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ProcessorConstraint ProcessorConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ResourceConstraint ResourceConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LaunchConstraint LaunchConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ColocationConstraint ColocationConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::ExecutionConstraintSet ExecutionConstraintSet;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::SpecializedConstraint SpecializedConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::MemoryConstraint MemoryConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::FieldConstraint FieldConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::OrderingConstraint OrderingConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::SplittingConstraint SplittingConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::DimensionConstraint DimensionConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::AlignmentConstraint AlignmentConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::OffsetConstraint OffsetConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::PointerConstraint PointerConstraint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LayoutConstraintSet LayoutConstraintSet;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::TaskLayoutConstraintSet TaskLayoutConstraintSet;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Runtime RealmRuntime;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Machine Machine;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Domain Domain;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::DomainPoint DomainPoint;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::IndexSpaceAllocator IndexSpaceAllocator;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::RegionInstance PhysicalInstance;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Memory Memory;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Processor Processor;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::CodeDescriptor CodeDescriptor;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Event Event;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Event MapperEvent;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::UserEvent UserEvent;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Reservation Reservation;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Barrier Barrier;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_reduction_op_id_t ReductionOpID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::ReductionOpUntyped ReductionOp;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_custom_serdez_id_t CustomSerdezID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::CustomSerdezUntyped SerdezOp;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Machine::ProcessorMemoryAffinity ProcessorMemoryAffinity;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::Machine::MemoryMemoryAffinity MemoryMemoryAffinity;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::ElementMask::Enumerator Enumerator;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Realm::IndexSpace::FieldDataDescriptor FieldDataDescriptor;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::CustomSerdezID, 
                     const Realm::CustomSerdezUntyped *> SerdezOpTable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Realm::ReductionOpID, 
            const Realm::ReductionOpUntyped *> ReductionOpTable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef void (*SerdezInitFnptr)(const Legion::ReductionOp*, 
                                    void *&, size_t&);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef void (*SerdezFoldFnptr)(const Legion::ReductionOp*, void *&, 
                                    size_t&, const void*, bool);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Realm::ReductionOpID, 
                     Legion::SerdezRedopFns> SerdezRedopTable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_address_space_t AddressSpace;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_task_priority_t TaskPriority;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_color_t Color;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_field_id_t FieldID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_trace_id_t TraceID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_mapper_id_t MapperID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_context_id_t ContextID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_instance_id_t InstanceID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_index_space_id_t IndexSpaceID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_index_partition_id_t IndexPartitionID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_index_tree_id_t IndexTreeID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_field_space_id_t FieldSpaceID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_generation_id_t GenerationID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_type_handle TypeHandle;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_projection_id_t ProjectionID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_region_tree_id_t RegionTreeID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_distributed_id_t DistributedID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_address_space_id_t AddressSpaceID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_tunable_id_t TunableID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_mapping_tag_id_t MappingTagID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_semantic_tag_t SemanticTag;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_variant_id_t VariantID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_unique_id_t UniqueID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_version_id_t VersionID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_task_id_t TaskID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef ::legion_layout_constraint_id_t LayoutConstraintID;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::Color,Legion::ColoredPoints<ptr_t> > Coloring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::Color,Legion::Domain> DomainColoring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::Color,
                     std::set<Legion::Domain> > MultiDomainColoring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::DomainPoint,
                     Legion::ColoredPoints<ptr_t> > PointColoring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::DomainPoint,Legion::Domain> DomainPointColoring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::DomainPoint,
                     std::set<Legion::Domain> > MultiDomainPointColoring;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef void (*RegistrationCallbackFnptr)(Realm::Machine machine, 
        Legion::Runtime *rt, const std::set<Legion::Processor> &local_procs);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LogicalRegion (*RegionProjectionFnptr)(
        Legion::LogicalRegion parent,
        const Legion::DomainPoint&, Legion::Runtime *rt);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::LogicalRegion (*PartitionProjectionFnptr)(
        Legion::LogicalPartition parent, 
        const Legion::DomainPoint&, Legion::Runtime *rt);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef bool (*PredicateFnptr)(const void*, size_t, 
        const std::vector<Legion::Future> futures);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::ProjectionID,Legion::RegionProjectionFnptr> 
      RegionProjectionTable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef std::map<Legion::ProjectionID,Legion::PartitionProjectionFnptr> 
      PartitionProjectionTable;
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef void (*RealmFnptr)(const void*,size_t,
                               const void*,size_t,Legion::Processor);
    LEGION_DEPRECATED("Use the Legion namespace instance instead.")
    typedef Legion::Internal::TaskContext* Context; 
  };
};

