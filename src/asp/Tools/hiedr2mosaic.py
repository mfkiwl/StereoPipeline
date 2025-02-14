#!/usr/bin/env python
# __BEGIN_LICENSE__
#  Copyright (c) 2009-2020, United States Government as represented by the
#  Administrator of the National Aeronautics and Space Administration. All
#  rights reserved.
#
#  The NGT platform is licensed under the Apache License, Version 2.0 (the
#  "License"); you may not use this file except in compliance with the
#  License. You may obtain a copy of the License at
#  http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
# __END_LICENSE__

from __future__ import print_function
import os, glob, optparse, re, shutil, subprocess, sys, string
try:
    from urllib2 import urlopen
except ImportError:  
    from urllib.request import urlopen

libexecpath = os.path.abspath(sys.path[0] + '/../libexec')
sys.path.insert(0, libexecpath) # prepend to Python path
from stereo_utils import get_asp_version

import asp_system_utils
asp_system_utils.verify_python_version_is_supported()

job_pool = []

def man(option, opt, value, parser):
    print(parser.usage, file=sys.stderr)
    print('''\
This program operates on HiRISE EDR (.IMG) channel files, and performs the
following ISIS 3 operations:
 * Converts to ISIS format (hi2isis)
 * Performs radiometric calibration (hical)
 * Stitches the channel files together into single CCD files (histitch)
 * Attaches SPICE information (spiceinit and spicefit)
 * Removes camera distortions from the CCD images (noproj)
 * Perfroms jitter analysis (hijitreg)
 * Mosaics individual CCDs into one unified image file (handmos)
 * Normalizes the mosaic (cubenorm)
''', file=sys.stderr)
    
    sys.exit()

class Usage(Exception):
    def __init__(self, msg):
        self.msg = msg

class CCDs(dict):
    ''''''
    def __init__(self, cubes, match=5):
        self.prefix = os.path.commonprefix( cubes )
        dict.__init__(self)
        for cub in cubes:
            number = int( cub[len(self.prefix)] )
            self[number] = cub
        self.match = int(match)

    def min(self):
        return min( self.keys() )

    def max(self):
        return max( self.keys() )

    def matchcube(self):
        return self[self.match]


def add_job( cmd, num_working_threads=4 ):
    if len(job_pool) >= num_working_threads:
        job_pool[0].wait()
        job_pool.pop(0)
    print(cmd)
    job_pool.append( subprocess.Popen(cmd, shell=True) )

def wait_on_all_jobs():
    print("Waiting for jobs to finish")
    while len(job_pool) > 0:
        job_pool[0].wait()
        job_pool.pop(0)

def read_flatfile( flat ):
    f = open(flat,'r')
    averages = [0.0,0.0]
    try:
        for line in f:
            if line.rfind("Average Sample Offset:") > 0:
                index       = line.rfind("Offset:")
                index_e     = line.rfind("StdDev:")
                crop        = line[index+7:index_e]
                averages[0] = float(crop)
            elif line.rfind("Average Line Offset:") > 0:
                index       = line.rfind("Offset:")
                index_e     = line.rfind("StdDev:")
                crop        = line[index+7:index_e]
                averages[1] = float(crop)
    except ValueError:
        print("Could not extract valid offsets from the flat file (" +
              flat + "). "
              "This could be because no matches were found. "
              "You may need to run hijitreg manually with a "
              "custom REGDEF parameter.  In order for this program "
              "to complete, we are returning zeros as the offset "
              "but this may result in misaligned CCDs.")
    return averages

def check_output_files(file_list):
    '''Verify the output files were created.'''
    for f in file_list:
        if not os.path.exists(f):
            raise Exception('Failed to generate file: ' + f)

def hi2isis( img_files, threads ):
    hi2isis_cubs = []
    for img in img_files:
        # Expect to end in .IMG, change to end in .cub
        to_cub = os.path.splitext( os.path.basename(img) )[0] + '.cub'
        if os.path.exists(to_cub):
            print(to_cub + ' exists, skipping hi2isis.')
        else:
            cmd = 'hi2isis from= '+ img +' to= '+ to_cub
            add_job(cmd, threads)
        hi2isis_cubs.append( to_cub )
    wait_on_all_jobs()
    check_output_files(hi2isis_cubs)
    return hi2isis_cubs

def hical( cub_files, threads, delete=False ):
    hical_cubs = []
    for cub in cub_files:
        # Expect to end in .cub, change to end in .hical.cub
        to_cub = os.path.splitext(cub)[0] + '.hical.cub'
        if os.path.exists(to_cub):
            print(to_cub + ' exists, skipping hical.')
        else:
            cmd = 'hical from=  '+ cub +' to= '+ to_cub
            add_job(cmd, threads)
        hical_cubs.append( to_cub )
    wait_on_all_jobs()
    check_output_files(hical_cubs)
    if delete:
        for cub in cub_files: os.remove( cub )
        hical_log_files = glob.glob( os.path.commonprefix(cub_files) + '*.hical.log' )
        for file in hical_log_files: os.remove( file )
    return hical_cubs

def histitch( cub_files, threads, delete=False ):
    histitch_cubs = []
    to_del_cubs   = []
    # Strictly, we should probably look in the image headers, but instead we'll
    # assume that people have kept a sane naming convention for their files, such
    # that the name consists of prefix + 'N_C' + suffix
    # where prefix we extract below and the 'N_C' string is where N is the CCD number
    # and C is the channel number.
    prefix = os.path.commonprefix( cub_files )
    channel_files = [[None]*2 for i in range(10)]
    pattern = re.compile(r"(\d)_(\d)")
    for cub in cub_files:
        match = re.match( pattern, cub[len(prefix):] )
        if match:
            ccd     = match.group(1)
            channel = match.group(2)
            # print ('ccd: ' + ccd + ' channel: '+ channel)
            channel_files[int(ccd)][int(channel)] = cub
        else:
            raise Exception( 'Could not find a CCD and channel identifier in ' + cub )

    for i in range(10):
        to_cub = prefix + str(i) + '.histitch.cub'

        if channel_files[i][0] and channel_files[i][1]:
            if os.path.exists(to_cub):
                print(to_cub + ' exists, skipping histitch.')
            else:
                cmd = 'histitch balance= TRUE from1= '+ channel_files[i][0] \
                        +' from2= '+ channel_files[i][1] +' to= '+ to_cub
                add_job(cmd, threads)
                to_del_cubs.append( channel_files[i][0] )
                to_del_cubs.append( channel_files[i][1] )
            histitch_cubs.append( to_cub )
        elif channel_files[i][0] or channel_files[i][1]:
            if channel_files[i][0]:
                found = channel_files[i][0]
            else:
                found = channel_files[i][1]
            print('Found '+ found  +' but not the matching channel file.')
            cmd = 'histitch from1= '+ found +' to= '+ to_cub
            add_job(cmd, threads)
            to_del_cubs.append( found )
            histitch_cubs.append( to_cub )

    wait_on_all_jobs()
    check_output_files(histitch_cubs)
    if delete:
        for cub in to_del_cubs: os.remove( cub )
    return histitch_cubs

def spice( cub_files, threads, web):
    for cub in cub_files:
        cmd = f'spiceinit WEB={web} from={cub}'
        add_job(cmd, threads)
    wait_on_all_jobs()
    for cub in cub_files:
        cmd = 'spicefit from= '+ cub
        add_job(cmd, threads)
    wait_on_all_jobs()
    return

def noproj( CCD_object, threads, delete=False ):
    noproj_CCDs = []
    for i in CCD_object.keys():
        to_cub = CCD_object.prefix + str(i) + '.noproj.cub'
        if os.path.exists( to_cub ):
            print(to_cub + ' exists, skipping noproj.')
        else:
            cmd = 'mkdir -p tmp_' + CCD_object[i] + '&& ' \
                + 'cd tmp_' + CCD_object[i] + '&& ' \
                + 'noproj from=../' + CCD_object[i] \
                + ' match=../' + CCD_object.matchcube() \
                + ' source= frommatch to=../'+ to_cub + '&& ' \
                + 'cd .. && rm -rf tmp_' + CCD_object[i]
            # cmd = 'noproj from= '+ CCD_object[i]    \
            #     +' match= '+ CCD_object.matchcube() \
            #     +' source= frommatch to= '+ to_cub
            add_job(cmd, threads)
            # print (cmd)
            # os.system(cmd)
        noproj_CCDs.append( to_cub )
    wait_on_all_jobs()
    if delete:
        for cub in CCD_object.values(): os.remove( cub )
    return CCDs( noproj_CCDs, CCD_object.match )

# Check for failure for hijitreg.  Sometimes bombs?  Default to zeros.
def hijitreg( noproj_CCDs, threads, delete=False ):
    for i in noproj_CCDs.keys():
        j = i + 1
        if( j not in noproj_CCDs ): continue
        cmd = 'hijitreg from= '+ noproj_CCDs[i]         \
            +' match= '+ noproj_CCDs[j]                 \
            + ' flatfile= flat_'+str(i)+'_'+str(j)+'.txt'
        add_job(cmd, threads)
    wait_on_all_jobs()

    averages = dict()

    for i in noproj_CCDs.keys():
        j = i + 1
        if( j not in noproj_CCDs ): continue
        flat_file = 'flat_'+str(i)+'_'+str(j)+'.txt'
        averages[i] = read_flatfile( flat_file )
        if delete:
            os.remove( flat_file )

    return averages

def mosaic( noprojed_CCDs, averages ):
    mosaic = noprojed_CCDs.prefix+'.mos_hijitreged.cub'
    shutil.copy( noprojed_CCDs.matchcube(), mosaic )
    sample_sum = 1
    line_sum   = 1
    for i in range( noprojed_CCDs.match-1, noprojed_CCDs.min()-1, -1):
        if i not in noprojed_CCDs: continue
        sample_sum  += averages[i][0]
        line_sum    += averages[i][1]
        handmos( noprojed_CCDs[i], mosaic,
                 str( int(round( sample_sum )) ),
                 str( int(round( line_sum )) ) )

    sample_sum = 1
    line_sum = 1
    for i in range( noprojed_CCDs.match+1, noprojed_CCDs.max()+1, 1):
        if i not in noprojed_CCDs: continue
        sample_sum  -= averages[i-1][0]
        line_sum    -= averages[i-1][1]
        handmos( noprojed_CCDs[i], mosaic,
                 str( int(round( sample_sum )) ),
                 str( int(round( line_sum )) ) )

    return mosaic


def handmos( fromcub, tocub, outsamp, outline ):
    cmd = 'handmos from= '+ fromcub +' mosaic= '+ tocub \
            +' outsample= '+ outsamp \
            +' outline= '+   outline \
            +' priority= beneath'
    os.system(cmd)
    return

def cubenorm( fromcub, delete=False ):
    tocub = os.path.splitext(fromcub)[0] + '.norm.cub'
    cmd   = 'cubenorm from= '+ fromcub+' to= '+ tocub
    print(cmd)
    os.system(cmd)
    if delete:
        os.remove( fromcub )
    return tocub


def fetch_files( url, output_folder, image_type='RED' ):
    '''Fetch all the files for a HiRISE image and return the list of files.'''

    os.system('mkdir -p ' + output_folder)

    # The URL should look like this:
    # https://hirise-pds.lpl.arizona.edu/PDS/EDR/ESP/ORB_052800_052899/ESP_052893_1835/

    try:
        # python2 
        from BeautifulSoup import BeautifulSoup
        parsedDataPage = BeautifulSoup(urlopen(url).read())
    except:
        # python3
        try:
            from bs4 import BeautifulSoup        
            parsedDataPage = BeautifulSoup(urlopen(url).read(), "html.parser")
        except Exception as e:
            raise Exception("Error: " + str(e) + "\n" + 
                            "If this is a missing module error, please install " + \
                            "BeautifulSoup for python 2 or bs4 for python 3.\n" +
                            "Or do not use the --download-folder option.")
        
    # Loop through all the links on the page
    image_list = []
    for link in parsedDataPage.findAll('a'):
        filename = link.string
        if image_type not in filename:
            continue # Skip types that don't match
        
        full_url   = os.path.join(url,           filename)
        local_path = os.path.join(output_folder, filename)
        image_list.append(local_path)

        # Download the file if it is not already there        
        if not os.path.exists(local_path):
            cmd = 'wget ' + full_url + ' -O ' + local_path
            print (cmd)
            os.system(cmd)
    
    return image_list

def get_ccd(path):
    '''Returns the CCD number of a filename.'''
    
    filename = os.path.basename(path)

    # Out of 'ESP_023957_1755/ESP_023957_1755_RED5_1.IMG', pull '_RED5_1',
    # and extract the number 5.
    match = re.match("^.*?RED(\d)", filename )
    if not match:
        raise Exception('Could not extract the CCD number from: ' + filename)
    ccd = int(match.group(1))
    return ccd

#----------------------------

def main():
    try:
        try:
            usage = "usage: hiedr2mosaic.py [--help][--manual][--threads N][--keep][-m match][-w bool] HiRISE-EDR.IMG-files\n  " + get_asp_version()
            parser = optparse.OptionParser(usage=usage)
            parser.set_defaults(delete=True)
            parser.set_defaults(match=5)
            parser.set_defaults(threads=4)
            parser.set_defaults(web=False)
            parser.add_option("--manual", action="callback", callback=man,
                              help="Read the manual.")
            parser.add_option("--stop-at-no-proj", dest="stop_no_proj", action="store_true",
                              help="Process the IMG files only to have SPICE attached. This allows jigsaw to happen")
            parser.add_option("--resume-at-no-proj", dest="resume_no_proj", action="store_true",
                              help="Pick back up after spiceinit has happened or jigsaw. This was noproj uses your new camera information")
            parser.add_option("-t", "--threads", dest="threads",
                              help="Number of threads to use.",type="int")
            parser.add_option("-m", "--match", dest="match",type="int",
                              help="CCD number of match CCD, passed as the match argument to noproj (default 5).")
            parser.add_option("-w", "--web", action="store_true", dest="web",
                              help="Invokes spiceinit with web=true, to fetch the kernels from the web.")
            parser.add_option("-k", "--keep", action="store_false",
                              dest="delete",
                              help="Will not delete intermediate files.")
            parser.add_option("--download-folder", dest="download_folder", default=None,
                              help="Download files to this folder. Hence the second argument to this is the URL of the page to download the files from.")
            

            (options, args) = parser.parse_args()

            if not args: 
                parser.error("need .IMG files or a URL")
            
            if options.download_folder:
                print ('Downloading HiRISE images from URL...')
                # For now we only fetch RED images.
                args = fetch_files(args[0], options.download_folder, image_type='RED')
                print ('Finished downloading ' + str(len(args)) + ' images.')

            # Verify that we have both of the specified match CCD's.
            # This is applicable only when we start with initial IMG files.
            if not options.resume_no_proj:            
                matchCount = 0
                for a in args:
                    if get_ccd(a) == options.match:
                        matchCount += 1
                        
                if matchCount != 2:
                    print ('Error: Found ' + str(matchCount) + ' files for match CCD ' 
                           + str(options.match) + ' instead of 2.')
                    return -1

            numCcds = len(args) / 2

        except optparse.OptionError as msg:
            raise Usage(msg)

        if not options.resume_no_proj:
            # hi2isis
            hi2isised = hi2isis( args, options.threads )

            # hical
            hicaled = hical( hi2isised, options.threads, options.delete )

            # histitch
            histitched = histitch( hicaled, options.threads, options.delete )

            # attach spice
            spice( histitched, options.threads, options.web)

        if options.stop_no_proj:
            print("Finished")
            return 0

        if options.resume_no_proj:
            histitched = args

        CCD_files = CCDs( histitched, options.match )

        # noproj
        noprojed_CCDs = noproj( CCD_files, options.threads, options.delete )

        # hijitreg
        averages = hijitreg( noprojed_CCDs, options.threads, options.delete )

        # mosaic handmos
        mosaicked = mosaic( noprojed_CCDs, averages )

        # Clean up noproj files
        if options.delete:
          for cub in noprojed_CCDs.values():
              os.remove( cub )

        # Run a final cubenorm across the image:
        cubenorm( mosaicked, options.delete )

        print("Finished")
        return 0

    except Usage as err:
        print (err.msg, file = sys.stderr)
        return 2

    # To more easily debug this program, comment out this catch block.
    # except Exception as err:
    #     sys.stderr.write( str(err) + '\n' )
    #     return 1


if __name__ == "__main__":
    sys.exit(main())
