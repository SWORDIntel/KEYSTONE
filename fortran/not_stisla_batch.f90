module not_stisla_batch_backend
  use, intrinsic :: iso_c_binding
  implicit none

contains

  subroutine not_stisla_batch_search_i64(data, n, keys, key_count, out_indices) &
      bind(C, name="not_stisla_batch_search_i64")
    type(c_ptr), value :: data
    integer(c_size_t), value :: n
    type(c_ptr), value :: keys
    integer(c_size_t), value :: key_count
    type(c_ptr), value :: out_indices

    integer(c_int64_t), pointer :: data_arr(:)
    integer(c_int64_t), pointer :: key_arr(:)
    integer(c_int64_t), pointer :: out_arr(:)
    integer(c_size_t) :: i

    if (.not. c_associated(out_indices)) return
    if (key_count == 0_c_size_t) return

    call c_f_pointer(out_indices, out_arr, [key_count])

    if ((.not. c_associated(data)) .or. (.not. c_associated(keys)) .or. &
        n == 0_c_size_t) then
      out_arr = -1_c_int64_t
      return
    end if

    call c_f_pointer(data, data_arr, [n])
    call c_f_pointer(keys, key_arr, [key_count])

    if (should_use_merge_walk(data_arr, n, key_arr, key_count)) then
      call merge_walk_search_i64(data_arr, n, key_arr, key_count, out_arr)
    else
      !$omp parallel do default(none) private(i) shared(data_arr, key_arr, out_arr, n, key_count) schedule(static)
      do i = 1_c_size_t, key_count
        out_arr(i) = binary_search_i64(data_arr, n, key_arr(i))
      end do
      !$omp end parallel do
    end if
  end subroutine not_stisla_batch_search_i64

  function should_use_merge_walk(arr, n, keys, key_count) result(use_merge)
    integer(c_int64_t), intent(in) :: arr(:)
    integer(c_size_t), value :: n
    integer(c_int64_t), intent(in) :: keys(:)
    integer(c_size_t), value :: key_count
    logical :: use_merge
    integer(c_size_t) :: i
    integer(c_size_t) :: start_pos
    integer(c_size_t) :: end_pos
    integer(c_size_t) :: range_span
    integer(c_size_t) :: binary_budget
    integer(c_size_t) :: log_n

    use_merge = .false.
    if (key_count < 64_c_size_t) return

    do i = 2_c_size_t, key_count
      if (keys(i) < keys(i - 1_c_size_t)) return
    end do

    start_pos = lower_bound_i64(arr, n, keys(1))
    end_pos = lower_bound_i64(arr, n, keys(key_count))

    if (end_pos < start_pos) return
    range_span = end_pos - start_pos + 1_c_size_t
    log_n = ceil_log2_size(n)
    binary_budget = key_count * log_n

    use_merge = (range_span + key_count <= binary_budget)
  end function should_use_merge_walk

  subroutine merge_walk_search_i64(arr, n, keys, key_count, out)
    integer(c_int64_t), intent(in) :: arr(:)
    integer(c_size_t), value :: n
    integer(c_int64_t), intent(in) :: keys(:)
    integer(c_size_t), value :: key_count
    integer(c_int64_t), intent(out) :: out(:)
    integer(c_size_t) :: i
    integer(c_size_t) :: ai
    integer(c_size_t) :: first_pos
    logical :: has_duplicate

    ai = lower_bound_i64(arr, n, keys(1))

    do i = 1_c_size_t, key_count
      do while (ai <= n)
        if (arr(ai) >= keys(i)) exit
        ai = ai + 1_c_size_t
      end do

      if (ai <= n .and. arr(ai) == keys(i)) then
        first_pos = ai
        has_duplicate = .false.
        if (first_pos > 1_c_size_t) then
          if (arr(first_pos - 1_c_size_t) == keys(i)) then
            has_duplicate = .true.
          end if
        end if
        if (.not. has_duplicate .and. first_pos < n) then
          if (arr(first_pos + 1_c_size_t) == keys(i)) then
            has_duplicate = .true.
          end if
        end if
        if (has_duplicate) then
          out(i) = binary_search_i64(arr, n, keys(i))
        else
          out(i) = int(first_pos - 1_c_size_t, c_int64_t)
        end if
      else
        out(i) = -1_c_int64_t
      end if
    end do
  end subroutine merge_walk_search_i64

  function lower_bound_i64(arr, n, key) result(pos)
    integer(c_int64_t), intent(in) :: arr(:)
    integer(c_size_t), value :: n
    integer(c_int64_t), value :: key
    integer(c_size_t) :: pos
    integer(c_size_t) :: lo
    integer(c_size_t) :: hi
    integer(c_size_t) :: mid

    lo = 1_c_size_t
    hi = n + 1_c_size_t

    do while (lo < hi)
      mid = lo + (hi - lo) / 2_c_size_t
      if (arr(mid) < key) then
        lo = mid + 1_c_size_t
      else
        hi = mid
      end if
    end do

    pos = lo
  end function lower_bound_i64

  function ceil_log2_size(n) result(log_n)
    integer(c_size_t), value :: n
    integer(c_size_t) :: log_n
    integer(c_size_t) :: value

    log_n = 1_c_size_t
    value = 1_c_size_t

    do while (value < n)
      value = value * 2_c_size_t
      log_n = log_n + 1_c_size_t
    end do
  end function ceil_log2_size

  function binary_search_i64(arr, n, key) result(index)
    integer(c_int64_t), intent(in) :: arr(:)
    integer(c_size_t), value :: n
    integer(c_int64_t), value :: key
    integer(c_int64_t) :: index
    integer(c_size_t) :: lo
    integer(c_size_t) :: hi
    integer(c_size_t) :: mid

    index = -1_c_int64_t
    lo = 1_c_size_t
    hi = n

    do while (lo <= hi)
      mid = lo + (hi - lo) / 2_c_size_t

      if (arr(mid) == key) then
        index = int(mid - 1_c_size_t, c_int64_t)
        return
      else if (arr(mid) < key) then
        lo = mid + 1_c_size_t
      else
        if (mid == 1_c_size_t) return
        hi = mid - 1_c_size_t
      end if
    end do
  end function binary_search_i64

end module not_stisla_batch_backend
