/* -*- c++ -*- */
/*
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdexcept>
#include <iostream>
#include <algorithm>
#ifdef USE_AVX
#include <immintrin.h>
#elif USE_SSE2
#include <emmintrin.h>
#endif

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/detail/endian.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>

#include <gnuradio/gr_io_signature.h>

#include "hackrf_sink_c.h"

#include "osmosdr_arg_helpers.h"

using namespace boost::assign;

#define BUF_LEN  (16 * 32 * 512) /* must be multiple of 512 */
#define BUF_NUM   32

#define BYTES_PER_SAMPLE  2 /* HackRF device consumes 8 bit unsigned IQ data */

static inline bool cb_init(circular_buffer_t *cb, size_t capacity, size_t sz)
{
  cb->buffer = malloc(capacity * sz);
  if(cb->buffer == NULL)
    return false; // handle error
  cb->buffer_end = (char *)cb->buffer + capacity * sz;
  cb->capacity = capacity;
  cb->count = 0;
  cb->sz = sz;
  cb->head = cb->buffer;
  cb->tail = cb->buffer;
  return true;
}

static inline void cb_free(circular_buffer_t *cb)
{
  if (cb->buffer) {
    free(cb->buffer);
    cb->buffer = NULL;
  }
  // clear out other fields too, just to be safe
  cb->buffer_end = 0;
  cb->capacity = 0;
  cb->count = 0;
  cb->sz = 0;
  cb->head = 0;
  cb->tail = 0;
}

static inline bool cb_has_room(circular_buffer_t *cb)
{
  if(cb->count == cb->capacity)
    return false;
  return true;
}

static inline bool cb_push_back(circular_buffer_t *cb, const void *item)
{
  if(cb->count == cb->capacity)
    return false; // handle error
  memcpy(cb->head, item, cb->sz);
  cb->head = (char *)cb->head + cb->sz;
  if(cb->head == cb->buffer_end)
    cb->head = cb->buffer;
  cb->count++;
  return true;
}

static inline bool cb_pop_front(circular_buffer_t *cb, void *item)
{
  if(cb->count == 0)
    return false; // handle error
  memcpy(item, cb->tail, cb->sz);
  cb->tail = (char *)cb->tail + cb->sz;
  if(cb->tail == cb->buffer_end)
    cb->tail = cb->buffer;
  cb->count--;
  return true;
}

int hackrf_sink_c::_usage = 0;
boost::mutex hackrf_sink_c::_usage_mutex;

hackrf_sink_c_sptr make_hackrf_sink_c (const std::string & args)
{
  return gnuradio::get_initial_sptr(new hackrf_sink_c (args));
}

/*
 * Specify constraints on number of input and output streams.
 * This info is used to construct the input and output signatures
 * (2nd & 3rd args to gr_block's constructor).  The input and
 * output signatures are used by the runtime system to
 * check that a valid number and type of inputs and outputs
 * are connected to this block.  In this case, we accept
 * only 0 input and 1 output.
 */
static const int MIN_IN = 1;  // mininum number of input streams
static const int MAX_IN = 1;  // maximum number of input streams
static const int MIN_OUT = 0;  // minimum number of output streams
static const int MAX_OUT = 0;  // maximum number of output streams

/*
 * The private constructor
 */
hackrf_sink_c::hackrf_sink_c (const std::string &args)
  : gr_sync_block ("hackrf_sink_c",
        gr_make_io_signature (MIN_IN, MAX_IN, sizeof (gr_complex)),
        gr_make_io_signature (MIN_OUT, MAX_OUT, sizeof (gr_complex))),
    _dev(NULL),
    _buf(NULL),
    _sample_rate(0),
    _center_freq(0),
    _freq_corr(0),
    _auto_gain(false),
    _amp_gain(0),
    _vga_gain(0)
{
  int ret;
  uint16_t val;

  dict_t dict = params_to_dict(args);

  _buf_num = 0;

  if (dict.count("buffers"))
    _buf_num = boost::lexical_cast< unsigned int >( dict["buffers"] );

  if (0 == _buf_num)
    _buf_num = BUF_NUM;

  {
    boost::mutex::scoped_lock lock( _usage_mutex );

    if ( _usage == 0 )
      hackrf_init(); /* call only once before the first open */

    _usage++;
  }

  _dev = NULL;
  ret = hackrf_open( &_dev );
  if (ret != HACKRF_SUCCESS)
    throw std::runtime_error("Failed to open HackRF device.");

  uint8_t board_id;
  ret = hackrf_board_id_read( _dev, &board_id );
  if (ret != HACKRF_SUCCESS)
    throw std::runtime_error("Failed to get board id.");

  char version[40];
  memset(version, 0, sizeof(version));
  ret = hackrf_version_string_read( _dev, version, sizeof(version));
  if (ret != HACKRF_SUCCESS)
    throw std::runtime_error("Failed to read version string.");
#if 0
  read_partid_serialno_t serial_number;
  ret = hackrf_board_partid_serialno_read( _dev, &serial_number );
  if (ret != HACKRF_SUCCESS)
    throw std::runtime_error("Failed to read serial number.");
#endif
  std::cerr << "Using " << hackrf_board_id_name(hackrf_board_id(board_id)) << " "
            << "with firmware " << version << " "
            << std::endl;

  if ( BUF_NUM != _buf_num ) {
    std::cerr << "Using " << _buf_num << " buffers of size " << BUF_LEN << "."
              << std::endl;
  }

  set_sample_rate( 5000000 );

  set_gain( 0 ); /* disable AMP gain stage */

  hackrf_max2837_read( _dev, 29, &val );
  val |= 0x3; /* enable TX VGA control over SPI */
  hackrf_max2837_write( _dev, 29, val );

  set_if_gain( 16 ); /* preset to a reasonable default (non-GRC use case) */

  _buf = (unsigned char *) malloc( BUF_LEN );

  cb_init( &_cbuf, _buf_num, BUF_LEN );

//  _thread = gruel::thread(_hackrf_wait, this);
}

/*
 * Our virtual destructor.
 */
hackrf_sink_c::~hackrf_sink_c ()
{
  if (_dev) {
//    _thread.join();
    hackrf_close( _dev );
    _dev = NULL;

    {
      boost::mutex::scoped_lock lock( _usage_mutex );

       _usage--;

      if ( _usage == 0 )
        hackrf_exit(); /* call only once after last close */
    }
  }

  if (_buf) {
    free(_buf);
    _buf = NULL;
  }

  cb_free( &_cbuf );
}

int hackrf_sink_c::_hackrf_tx_callback(hackrf_transfer *transfer)
{
  hackrf_sink_c *obj = (hackrf_sink_c *)transfer->tx_ctx;
  return obj->hackrf_tx_callback(transfer->buffer, transfer->valid_length);
}

int hackrf_sink_c::hackrf_tx_callback(unsigned char *buffer, uint32_t length)
{
#if 0
  for (unsigned int i = 0; i < length; ++i) /* simulate noise */
    *buffer++ = rand() % 255;
#else
  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    if ( ! cb_pop_front( &_cbuf, buffer ) ) {
      memset(buffer, 0, length);
      std::cerr << "U" << std::flush;
    } else {
//      std::cerr << ":" << std::flush;
      _buf_cond.notify_one();
    }
  }

#endif

  return 0; // TODO: return -1 on error/stop
}

void hackrf_sink_c::_hackrf_wait(hackrf_sink_c *obj)
{
  obj->hackrf_wait();
}

void hackrf_sink_c::hackrf_wait()
{
}

bool hackrf_sink_c::start()
{
  if ( ! _dev )
    return false;

  _buf_used = 0;

  int ret = hackrf_start_tx( _dev, _hackrf_tx_callback, (void *)this );
  if (ret != HACKRF_SUCCESS) {
    std::cerr << "Failed to start TX streaming (" << ret << ")" << std::endl;
    return false;
  }

  while ( ! hackrf_is_streaming( _dev ) );

  return (bool) hackrf_is_streaming( _dev );
}

bool hackrf_sink_c::stop()
{
  if ( ! _dev )
    return false;

  int ret = hackrf_stop_tx( _dev );
  if (ret != HACKRF_SUCCESS) {
    std::cerr << "Failed to stop TX streaming (" << ret << ")" << std::endl;
    return false;
  }

  while ( hackrf_is_streaming( _dev ) );

  /* FIXME: hackrf_stop_tx should wait until the device is ready for a start */
  /* required if we want to immediately start() again */
  boost::this_thread::sleep( boost::posix_time::milliseconds(100) );

  return ! (bool) hackrf_is_streaming( _dev );
}


#ifdef USE_AVX
void convert_avx(const float* inbuf, unsigned char* outbuf,const unsigned int count)
{
  __m256 mulme = _mm256_set_ps(127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f, 127.0f);
  __m128i addme = _mm_set_epi16(127, 127, 127, 127, 127, 127, 127, 127);

  for(unsigned int i=0; i<count;i++){

  __m256i itmp3 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(&inbuf[i*16+0]), mulme));
  __m256i itmp4 = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(&inbuf[i*16+8]), mulme));

  __m128i a1 = _mm256_extractf128_si256(itmp3, 1);
  __m128i a0 = _mm256_castsi256_si128(itmp3);
  __m128i a3 = _mm256_extractf128_si256(itmp4, 1);
  __m128i a2 = _mm256_castsi256_si128(itmp4);

  __m128i outshorts1 = _mm_add_epi16(_mm_packs_epi32(a0, a1), addme);
  __m128i outshorts2 = _mm_add_epi16(_mm_packs_epi32(a2, a3), addme);
  __m128i outbytes = _mm_packus_epi16(outshorts1, outshorts2);

  _mm_storeu_si128 ((__m128i*)&outbuf[i*16], outbytes);
  }
}

#elif USE_SSE2
void convert_sse2(const float* inbuf, unsigned char* outbuf,const unsigned int count)
{
  const register __m128 mulme = _mm_set_ps( 127.0f, 127.0f, 127.0f, 127.0f );
  __m128i addme = _mm_set_epi16( 127, 127, 127, 127, 127, 127, 127, 127);
  __m128 itmp1,itmp2,itmp3,itmp4;
  __m128i otmp1,otmp2,otmp3,otmp4;

  __m128i outshorts1,outshorts2;
  __m128i outbytes;

  for(unsigned int i=0; i<count;i++){

  itmp1 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+0]), mulme);
  itmp2 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+4]), mulme);
  itmp3 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+8]), mulme);
  itmp4 = _mm_mul_ps(_mm_loadu_ps(&inbuf[i*16+12]), mulme);

  otmp1 = _mm_cvtps_epi32(itmp1);
  otmp2 = _mm_cvtps_epi32(itmp2);
  otmp3 = _mm_cvtps_epi32(itmp3);
  otmp4 = _mm_cvtps_epi32(itmp4);

  outshorts1 = _mm_add_epi16(_mm_packs_epi32(otmp1, otmp2), addme);
  outshorts2 = _mm_add_epi16(_mm_packs_epi32(otmp3, otmp4), addme);

  outbytes = _mm_packus_epi16(outshorts1, outshorts2);

  _mm_storeu_si128 ((__m128i*)&outbuf[i*16], outbytes);
  }
}
#endif

void convert_default(float* inbuf, unsigned char* outbuf,const unsigned int count)
{
  for(unsigned int i=0; i<count;i++){
    outbuf[i]= inbuf[i]*127+127;
  }
}

int hackrf_sink_c::work( int noutput_items,
  gr_vector_const_void_star &input_items,
  gr_vector_void_star &output_items )
{
  const gr_complex *in = (const gr_complex *) input_items[0];

  {
    boost::mutex::scoped_lock lock( _buf_mutex );

    while ( ! cb_has_room(&_cbuf) )
      _buf_cond.wait( lock );
  }

  unsigned char *buf = _buf + _buf_used;
  unsigned int prev_buf_used = _buf_used;

  unsigned int remaining = (BUF_LEN-_buf_used)/2; //complex

  unsigned int count = std::min((unsigned int)noutput_items,remaining);
  unsigned int sse_rem = count/8; // 8 complex = 16f==512bit for avx
  unsigned int nosse_rem = count%8; // remainder

#ifdef USE_AVX
  convert_avx((float*)in, buf, sse_rem);
  convert_default((float*)(in+sse_rem*8), buf+(sse_rem*8*2), nosse_rem*2);
#elif USE_SSE2
  convert_sse2((float*)in, buf, sse_rem);
  convert_default((float*)(in+sse_rem*8), buf+(sse_rem*8*2), nosse_rem*2);
#else
  convert_default((float*)in, buf, count*2);
#endif

  _buf_used += (sse_rem*8+nosse_rem)*2;
  int items_consumed = sse_rem*8+nosse_rem;

  if(noutput_items >= remaining) {
    {
      boost::mutex::scoped_lock lock( _buf_mutex );

      if ( ! cb_push_back( &_cbuf, _buf ) ) {
        _buf_used = prev_buf_used;
        items_consumed = 0;
        std::cerr << "O" << std::flush;
      } else {
        //          std::cerr << "." << std::flush;
        _buf_used = 0;
      }
    }
  }

  // Tell runtime system how many input items we consumed on
  // each input stream.
  consume_each(items_consumed);

  // Tell runtime system how many output items we produced.
  return 0;
}

std::vector<std::string> hackrf_sink_c::get_devices()
{
  std::vector<std::string> devices;
  std::string label;

  for (unsigned int i = 0; i < 1 /* TODO: missing libhackrf api */; i++) {
    std::string args = "hackrf=" + boost::lexical_cast< std::string >( i );

    label.clear();

    label = "HackRF Jawbreaker"; /* TODO: missing libhackrf api */

    boost::algorithm::trim(label);

    args += ",label='" + label + "'";
    devices.push_back( args );
  }

  return devices;
}

size_t hackrf_sink_c::get_num_channels()
{
  return 1;
}

osmosdr::meta_range_t hackrf_sink_c::get_sample_rates()
{
  osmosdr::meta_range_t range;

  range += osmosdr::range_t( 5e6 ); /* out of spec but appears to work */
  range += osmosdr::range_t( 10e6 );
  range += osmosdr::range_t( 12.5e6 );
  range += osmosdr::range_t( 16e6 );
  range += osmosdr::range_t( 20e6 ); /* confirmed to work on fast machines */

  return range;
}

double hackrf_sink_c::set_sample_rate(double rate)
{
  int ret;

  if (_dev) {
    ret = hackrf_sample_rate_set( _dev, uint32_t(rate) );
    if ( HACKRF_SUCCESS == ret ) {
      _sample_rate = rate;
      set_bandwidth( rate );
    } else {
      throw std::runtime_error( std::string( __FUNCTION__ ) + " has failed" );
    }
  }

  return get_sample_rate();
}

double hackrf_sink_c::get_sample_rate()
{
  return _sample_rate;
}

osmosdr::freq_range_t hackrf_sink_c::get_freq_range( size_t chan )
{
  osmosdr::freq_range_t range;

  range += osmosdr::range_t( 30e6, 6e9 );

  return range;
}

double hackrf_sink_c::set_center_freq( double freq, size_t chan )
{
  int ret;

  #define APPLY_PPM_CORR(val, ppm) ((val) * (1.0 + (ppm) * 0.000001))

  if (_dev) {
    double corr_freq = APPLY_PPM_CORR( freq, _freq_corr );
    ret = hackrf_set_freq( _dev, uint64_t(corr_freq) );
    if ( HACKRF_SUCCESS == ret ) {
      _center_freq = freq;
    } else {
      throw std::runtime_error( std::string( __FUNCTION__ ) + " has failed" );
    }
  }

  return get_center_freq( chan );
}

double hackrf_sink_c::get_center_freq( size_t chan )
{
  return _center_freq;
}

double hackrf_sink_c::set_freq_corr( double ppm, size_t chan )
{
  _freq_corr = ppm;

  set_center_freq( _center_freq );

  return get_freq_corr( chan );
}

double hackrf_sink_c::get_freq_corr( size_t chan )
{
  return _freq_corr;
}

std::vector<std::string> hackrf_sink_c::get_gain_names( size_t chan )
{
  std::vector< std::string > names;

  names += "RF";
  names += "IF";

  return names;
}

osmosdr::gain_range_t hackrf_sink_c::get_gain_range( size_t chan )
{
  return get_gain_range( "RF", chan );
}

osmosdr::gain_range_t hackrf_sink_c::get_gain_range( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return osmosdr::gain_range_t( 0, 14, 14 );
  }

  if ( "IF" == name ) {
    return osmosdr::gain_range_t( 0, 47, 1 );
  }

  return osmosdr::gain_range_t();
}

bool hackrf_sink_c::set_gain_mode( bool automatic, size_t chan )
{
  _auto_gain = automatic;

  return get_gain_mode(chan);
}

bool hackrf_sink_c::get_gain_mode( size_t chan )
{
  return _auto_gain;
}

double hackrf_sink_c::set_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t rf_gains = get_gain_range( "RF", chan );

  if (_dev) {
    double clip_gain = rf_gains.clip( gain, true );

    std::map<double, int> reg_vals;
    reg_vals[ 0 ] = 0;
    reg_vals[ 14 ] = 1;

    if ( reg_vals.count( clip_gain ) ) {
      int value = reg_vals[ clip_gain ];
#if 0
      std::cerr << "amp gain: " << gain
                << " clip_gain: " << clip_gain
                << " value: " << value
                << std::endl;
#endif
      if ( hackrf_set_amp_enable( _dev, value ) == HACKRF_SUCCESS )
        _amp_gain = clip_gain;
    }
  }

  return _amp_gain;
}

double hackrf_sink_c::set_gain( double gain, const std::string & name, size_t chan)
{
  if ( "RF" == name ) {
    return set_gain( gain, chan );
  }

  if ( "IF" == name ) {
    return set_if_gain( gain, chan );
  }

  return set_gain( gain, chan );
}

double hackrf_sink_c::get_gain( size_t chan )
{
  return _amp_gain;
}

double hackrf_sink_c::get_gain( const std::string & name, size_t chan )
{
  if ( "RF" == name ) {
    return get_gain( chan );
  }

  if ( "IF" == name ) {
    return _vga_gain;
  }

  return get_gain( chan );
}

double hackrf_sink_c::set_if_gain( double gain, size_t chan )
{
  osmosdr::gain_range_t if_gains = get_gain_range( "IF", chan );

  double clip_gain = if_gains.clip( gain, true );
  double rel_atten = fabs( if_gains.stop() - clip_gain );

  std::vector< osmosdr::gain_range_t > if_attens;

  if_attens += osmosdr::gain_range_t(0, 1, 1); /* chapter 1.5: TX Gain Control */
  if_attens += osmosdr::gain_range_t(0, 2, 2);
  if_attens += osmosdr::gain_range_t(0, 4, 4);
  if_attens += osmosdr::gain_range_t(0, 8, 8);
  if_attens += osmosdr::gain_range_t(0, 16, 16);
  if_attens += osmosdr::gain_range_t(0, 16, 16);

  std::map< int, double > attens;

  /* initialize with min attens */
  for (unsigned int i = 0; i < if_attens.size(); i++) {
    attens[ i + 1 ] = if_attens[ i ].start();
  }

  double atten = rel_atten;

  for (int i = if_attens.size() - 1; i >= 0; i--) {
    osmosdr::gain_range_t range = if_attens[ i ];

    if ( atten - range.stop() >= 0 ) {
      atten -= range.stop();
      attens[ i + 1 ] = range.stop();
    }
  }
#if 0
  std::cerr << rel_atten << " => "; double sum = 0;
  for (unsigned int i = 0; i < attens.size(); i++) {
    sum += attens[ i + 1 ];
    std::cerr << attens[ i + 1 ] << " ";
  }
  std::cerr << " = " << sum << std::endl;
#endif
  if (_dev) {
    int value = 0;
    for (unsigned int stage = 1; stage <= attens.size(); stage++) {
      if ( attens[ stage ] != 0 )
        value |= 1 << (stage - 1);
    }
#if 0
    std::cerr << "vga gain: " << gain
              << " clip_gain: " << clip_gain
              << " rel_atten: " << rel_atten
              << " value: " << value
              << std::endl;
#endif
    uint16_t val;
    hackrf_max2837_read( _dev, 29, &val );

    val = (val & 0xf) | ((value & 0x3f) << 4);

    if ( hackrf_max2837_write( _dev, 29, val ) == HACKRF_SUCCESS )
      _vga_gain = clip_gain;
  }

  return _vga_gain;
}

double hackrf_sink_c::set_bb_gain(double gain, size_t chan)
{
  return 0;
}

std::vector< std::string > hackrf_sink_c::get_antennas( size_t chan )
{
  std::vector< std::string > antennas;

  antennas += get_antenna( chan );

  return antennas;
}

std::string hackrf_sink_c::set_antenna( const std::string & antenna, size_t chan )
{
  return get_antenna( chan );
}

std::string hackrf_sink_c::get_antenna( size_t chan )
{
  return "TX/RX";
}

double hackrf_sink_c::set_bandwidth( double bandwidth, size_t chan )
{
  int ret;
//  osmosdr::freq_range_t bandwidths = get_bandwidth_range( chan );

  if ( bandwidth == 0.0 ) /* bandwidth of 0 means automatic filter selection */
    bandwidth = _sample_rate;

  if ( _dev ) {
    /* compute best default value depending on sample rate (auto filter) */
    uint32_t bw = hackrf_compute_baseband_filter_bw( uint32_t(bandwidth) );
    ret = hackrf_baseband_filter_bandwidth_set( _dev, bw );
    if ( HACKRF_SUCCESS == ret ) {
      _bandwidth = bw;
    } else {
      throw std::runtime_error( std::string( __FUNCTION__ ) + " has failed" );
    }
  }

  return _bandwidth;
}

double hackrf_sink_c::get_bandwidth( size_t chan )
{
  return _bandwidth;
}

osmosdr::freq_range_t hackrf_sink_c::get_bandwidth_range( size_t chan )
{
  osmosdr::freq_range_t bandwidths;

  // TODO: read out from libhackrf when an API is available

  bandwidths += osmosdr::range_t( 1750000 );
  bandwidths += osmosdr::range_t( 2500000 );
  bandwidths += osmosdr::range_t( 3500000 );
  bandwidths += osmosdr::range_t( 5000000 );
  bandwidths += osmosdr::range_t( 5500000 );
  bandwidths += osmosdr::range_t( 6000000 );
  bandwidths += osmosdr::range_t( 7000000 );
  bandwidths += osmosdr::range_t( 8000000 );
  bandwidths += osmosdr::range_t( 9000000 );
  bandwidths += osmosdr::range_t( 10000000 );
  bandwidths += osmosdr::range_t( 12000000 );
  bandwidths += osmosdr::range_t( 14000000 );
  bandwidths += osmosdr::range_t( 15000000 );
  bandwidths += osmosdr::range_t( 20000000 );
  bandwidths += osmosdr::range_t( 24000000 );
  bandwidths += osmosdr::range_t( 28000000 );

  return bandwidths;
}
