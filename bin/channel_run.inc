<?php
/* vim:set softtabstop=4 shiftwidth=4 expandtab: */
/**
 *
 * LICENSE: GNU General Public License, version 2 (GPLv2)
 * Copyright 2001 - 2015 Ampache.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License v2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

define('NO_SESSION','1');
define('CLI', 1);

$chunk_buffer = '';
$nb_chunks_remainder = 0;
$path = dirname(__FILE__);
$prefix = realpath($path . '/../');
require_once $prefix . '/lib/init.php';
ob_end_flush();

$metadata_interval = 16000;

$start_date = time();
$verbose = false;
$port = 0;
$chanid = 0;

$cargv = count($_SERVER['argv']);
if ($cargv > 1) {
    for ($x = 1; $x < $cargv; $x++) {

        if ($_SERVER['argv'][$x] == "-c" && ($x + 1) < $cargv) {
            $chanid = intval($_SERVER['argv'][++$x]);
            $operations_string .= "\n\t" . T_('- Channel ' . $chanid);
        }
        elseif ($_SERVER['argv'][$x] == "-v") {
            $operations_string .= "\n\t" . T_('- Verbose');
            $verbose = true;
        }
        elseif ($_SERVER['argv'][$x] == "-p" && ($x + 1) < $cargv) {
            $port = intval($_SERVER['argv'][++$x]);
            $operations_string .= "\n\t" . T_('- Port ' . $port);
        }
    }
}

if ($chanid <= 0) {
    usage();
    exit;
}

// Transcode is mandatory to have consistent stream codec
$transcode_cfg = AmpConfig::get('transcode');

if ($transcode_cfg == 'never') {
    die('Cannot start channel, transcoding is mandatory to work.');
}

echo T_("Starting Channel...") . $operations_string . "\n";

$channel = new Channel($chanid);
if (!$channel->id) {
    die (T_("Unknown channel."));
}

if ($port <= 0) {
    if ($channel->fixed_endpoint) {
        $address = $channel->interface;
        $port = $channel->port;
    } else {
        $address = "127.0.0.1";
        // Try to find an available port
        for ($p = 8200; $p < 8300; ++$p) {
            $connection = @fsockopen($address, $p);
            if (is_resource($connection)) {
                fclose($connection);
            } else {
                echo T_("Found available port ") . $p . "\n";
                $port = $p;
                break;
            }
        }
    }
}

ob_start();

$server_uri = 'tcp://' . $address . ':' . $port;
$server = stream_socket_server($server_uri, $errno, $errorMessage);
if ($server === false)
{
    die("Could not bind to socket: " . $errorMessage);
}
$channel->update_start($start_date, $address, $port, getmypid());
echo T_("Listening on ") . $address . ':' . $port . "\n";

$stream_clients = array();
$client_socks = array();
$last_stream = microtime(true);
while(true)
{
    //prepare readable sockets
    $read_socks = $client_socks;
    if (count($client_socks) < $channel->max_listeners) {
        $read_socks[] = $server;
    }
    //echo "b\n";ob_flush();
    //start reading and use a large timeout
    if(stream_select ( $read_socks, $write, $except, 1))
    {
        //new client
        if (in_array($server, $read_socks))
        {
            $new_client = stream_socket_accept($server);

            if ($new_client)
            {
                debug_event('channel', 'Connection accepted from ' . stream_socket_get_name($new_client, true) . '.', '5');
                $client_socks[] = $new_client;
                $channel->update_listeners(count($client_socks), true);
                debug_event('channel', 'Now there are total '. count($client_socks) . ' clients.', '5');
                echo "New client connected.\n";
                ob_flush();
            }

            //delete the server socket from the read sockets
            unset($read_socks[array_search($server, $read_socks)]);
        }

        // Get new message from existing client
        foreach($read_socks as $sock)
        {
            // Handle data parse
            http_serve($channel, $client_socks, $stream_clients, $read_socks, $sock);
        }
    }

    if ($channel->bitrate) {

        $time_offset = microtime(true) - $last_stream;

        //debug_event('channel', 'time_offset : '. $time_offset, '5');
        //debug_event('channel', 'last_stream: '.$last_stream, '5');

        if ($time_offset < 1)
            usleep(1000000 - ($time_offset * 1000000)); // always at least 1 second between cycles

        $last_stream = microtime(true);
        $mtime = ($time_offset > 1) ? $time_offset : 1;
        $nb_chunks = ceil(($mtime * ($channel->bitrate+1/100*$channel->bitrate) * 1000 / 8) / $channel->chunk_size); // channel->bitrate+1% ... leave some headroom for metadata / headers

        // we only send full blocks, save remainder and apply when appropriate: allows more granular/arbitrary average bitrates
        if ($nb_chunks - ($mtime * ($channel->bitrate+1/100*$channel->bitrate) * 1000 / 8 / $channel->chunk_size) > 0)
            $nb_chunks_remainder += $nb_chunks - ($mtime * $channel->bitrate * 1000 / 8 / $channel->chunk_size);
        if ($nb_chunks >= 1 && $nb_chunks_remainder >= 1){
            $nb_chunks -= 1;
            $nb_chunks_remainder -= 1;
            //debug_event('channel', 'REMAINDER: '.$nb_chunks_remainder, '5');
        }
        //debug_event('channel', 'mtime '.$mtime, '5');
        //debug_event('channel', 'nb_chunks: '.$nb_chunks, '5');

    } else {
        $nb_chunks = 1;
    }

    // Get multiple chunks according to bitrate to return enough data per second (because sleep with socket select)
    for ($c = 0; $c < $nb_chunks; $c++)  {

        $chunk = $channel->get_chunk();
        $chunklen = strlen($chunk);
        $chunk_buffer .= $chunk;

        //buffer maintenance
        while (strlen($chunk_buffer) > (15 * $nb_chunks * $channel->chunk_size) ){ // buffer 15 seconds

            if (strtolower($channel->stream_type) == "ogg" && strtohex(substr($chunk_buffer, 0, 4)) == "4F676753") { //maintain ogg chunk alignment --- "4F676753" == "OggS"
                // read OggS segment length
                $hex = strtohex(substr($chunk_buffer, 0, 27));
                $ogg_nr_of_segments = hexdec(substr($hex, 26*2, 2));
                $hex .= strtohex(substr($chunk_buffer, 27, $ogg_nr_of_segments));
                $ogg_sum_segm_laces = 0;
                for($segm = 0; $segm < $ogg_nr_of_segments; $segm++){
                    $ogg_sum_segm_laces += hexdec(substr($hex, 27*2 + $segm*2, 2));
                }
                //$naive = strpos(substr($chunk_buffer, 4), 'OggS') + 4; // naive search for next header
                //remove 1 whole OggS chunk
                $chunk_buffer = substr($chunk_buffer, 27 + $ogg_nr_of_segments + $ogg_sum_segm_laces);
                //debug_event('channel', '$new chunk buffer : '.substr($chunk_buffer,0,300) . ' $hex: '.strtohex(substr($chunk_buffer,0,600)) . ' $ogg_nr_of_segments: ' .$ogg_nr_of_segments . ' bytes cut off: '.(27 + $ogg_nr_of_segments + $ogg_sum_segm_laces) . ' naive: ' .$naive, '5');
            } elseif (strtolower($channel->stream_type) == "ogg") {
                debug_event('channel', 'Ogg alignament broken! Trying repair...', '5');
                $manual_search = strpos($chunk_buffer, 'OggS');
                $chunk_buffer = substr($chunk_buffer, $manual_search);
            } else { // no chunk alignment required
                $chunk_buffer = substr($chunk_buffer, $chunklen);
            }
            //debug_event('channel', 'remvd chunk from buffer ', '5');
        }

        if ($chunklen > 0) {
            foreach($stream_clients as $key => $client)
            {
                $sock = $client['socket'];
                $clchunk = $chunk;

                if(!is_resource($sock)) {
                    client_disconnect($channel, $client_socks, $stream_clients, $sock);
                    continue;
                }

                if ($client['isnew'] == 1){
                    $client['isnew'] = 0;
                    //fwrite($sock, $channel->header_chunk);
                    //debug_event('channel', 'IS NEW' . $channel->header_chunk, '5');
                    $clchunk_buffer = $channel->header_chunk . $chunk_buffer;
                    if ($client['metadata']){ //stub
                        //if (strtolower($channel->stream_type) == "ogg")
                        while(strlen($clchunk_buffer) > $metadata_interval){
                            fwrite($sock, substr($clchunk_buffer, 0, $metadata_interval) . chr(0x00));
                            $clchunk_buffer = substr($clchunk_buffer, $metadata_interval);
                        }
                        fwrite($sock, $clchunk_buffer);
                        $client['metadata_lastsent'] = 0;
                        $client['length'] += strlen($clchunk_buffer);
                    } else {
                        //fwrite($sock, $channel->header_chunk);
                        $buffer_bytes_written = fwrite($sock, $clchunk_buffer);
                        while ($buffer_bytes_written != strlen($clchunk_buffer)){
                            debug_event('channel', 'I HERPED WHEN I SHOULD HAVE DERPED!', '5');
                            //debug_event('channel', 'chunk_buffer bytes written:' .$buffer_bytes_written .'strlen $chunk_buffer: '.strlen($chunk_buffer), '5');
                            $clchunk_buffer = substr($clchunk_buffer, $buffer_bytes_written);
                            $buffer_bytes_written = fwrite($sock, $clchunk_buffer);
                        }
                    }
                    $stream_clients[$key] = $client;
                    continue;
                }

                // Check if we need to insert metadata information
                if ($client['metadata']) {
                    $chkmdlen = ($client['length'] + $chunklen) - $client['metadata_lastsent'];
                    if ($chkmdlen >= $metadata_interval) {
                        $subpos = ($client['metadata_lastsent'] + $metadata_interval) - $client['length'];
                        fwrite($sock, substr($clchunk, 0, $subpos));
                        $client['length'] += $subpos;
                        if ($channel->media->id != $client['metadata_lastsong']) {
                            $metadata = "StreamTitle='" . str_replace('-', ' ', $channel->media->f_artist) . "-" . $channel->media->f_title . "';";
                            $metadata .= chr(0x00);
                            $metadatalen = ceil(strlen($metadata) / 16);
                            $metadata = str_pad($metadata, $metadatalen * 16, chr(0x00), STR_PAD_RIGHT);
                            //debug_event('channel', 'Sending metadata to client...', '5');
                            fwrite($sock, chr($metadatalen) . $metadata);
                            $client['metadata_lastsong'] = $channel->media->id;
                        } else {
                            fwrite($sock, chr(0x00));
                        }
                        $client['metadata_lastsent'] = $client['length'];
                        $clchunk = substr($chunk, $subpos);
                    }
                }

                if (strlen($clchunk) > 0) {
                    fwrite($sock, $clchunk);
                    $client['length'] += strlen($clchunk);
                }
                $stream_clients[$key] = $client;
                //debug_event('channel', 'Client stream current length: ' . $client['length'], '5');
            }
        } else {
            $channel->update_listeners(0);
            die('No more data, stream ended.');
        }
    }
}

ob_end_flush();
echo "\n";

function client_disconnect($channel, &$client_socks, &$stream_clients, $sock)
{
    $key = array_search($sock, $client_socks);
    unset($client_socks[$key]);
    unset($stream_clients[$key]);
    @fclose($sock);
    $channel->update_listeners(count($client_socks));
    debug_event('channel', 'A client disconnected. Now there are total '. count($client_socks) . ' clients.', '5');
    echo "Client disconnected.\n";
    ob_flush();
}

function usage()
{
    echo T_("- Channel Listening -");
    echo "\n";
    echo T_("Usage: channel_run.inc [-c {CHANNEL ID}|-p {PORT}|-v]");
    echo "\n\t";
    echo "\n-c {CHANNEL ID}\t";
    echo T_('Channel id to start');
    echo "\n-p {PORT}\t";
    echo T_('Listening port, default get an available port automatically');
    echo "\n-v\t";
    echo T_('Verbose');
    echo "\n";
    echo "----------------------------------------------------------";
    echo "\n";
}

function http_serve($channel, &$client_socks, &$stream_clients, &$read_socks, $sock)
{
    $data = fread($sock, 1024);
    if(!$data)
    {
        client_disconnect($channel, $client_socks, $stream_clients, $sock);
        return;
    }

    $headers = explode("\n", $data);

    if (count($headers) > 0) {
        $cmd = explode(" ", $headers[0]);
        if ($cmd['0'] == 'GET') {
            switch ($cmd['1']) {
                case '/stream.' . $channel->stream_type:
                    $options = array(
                            'socket' => $sock,
                            'length' => 0,
                            'isnew' => 1
                            );

                    //debug_event('channel', 'HTTP HEADERS: '.$data,'5');
                    for ($i = 1; $i < count($headers); $i++) {
                        $headerpart = explode(":", $headers[$i], 2);
                        $header = strtolower(trim($headerpart[0]));
                        $value = trim($headerpart[1]);
                        switch ($header) {
                            case 'icy-metadata':
                                $options['metadata'] = ($value == '1');
                                $options['metadata_lastsent'] = 0;
                                $options['metadata_lastsong'] = 0;
                                break;
                        }
                    }

                    // Stream request
                    if ($options['metadata']) {
                        //$http = "ICY 200 OK\r\n");
                        $http = "HTTP/1.0 200 OK\r\n";
                    } else  {
                        $http = "HTTP/1.1 200 OK\r\n";
                        $http .= "Cache-Control: no-store, no-cache, must-revalidate\r\n";
                    }
                    $http .= "Content-Type: " . Song::type_to_mime($channel->stream_type) . "\r\n";
                    $http .= "Accept-Ranges: none\r\n";

                    $genre = $channel->get_genre();
                    // Send Shoutcast metadata on demand
                    //if ($options['metadata']) {
                        $http .= "icy-notice1: " . AmpConfig::get('site_title') . "\r\n";
                        $http .= "icy-name: " . $channel->name . "\r\n";
                        if (!empty($genre)) {
                            $http .= "icy-genre: " . $genre . "\r\n";
                        }
                        $http .= "icy-url: " . $channel->url . "\r\n";
                        $http .= "icy-pub: " . (($channel->is_private) ? "0" : "1") . "\r\n";
                        if ($channel->bitrate) {
                            $http .= "icy-br: " . strval($channel->bitrate) . "\r\n";
                        }
                        global $metadata_interval;
                        $http .= "icy-metaint: " . strval($metadata_interval) . "\r\n";
                    //}
                    // Send additional Icecast metadata
                    $http .= "x-audiocast-server-url: " . $channel->url . "\r\n";
                    $http .= "x-audiocast-name: " . $channel->name . "\r\n";
                    $http .= "x-audiocast-description: " . $channel->description . "\r\n";
                    $http .= "x-audiocast-url: " . $channel->url . "\r\n";
                    if (!empty($genre)) {
                        $http .= "x-audiocast-genre: " . $genre . "\r\n";
                    }
                    $http .= "x-audiocast-bitrate: " . strval($channel->bitrate) . "\r\n";
                    $http .= "x-audiocast-public: " . (($channel->is_private) ? "0" : "1") . "\r\n";

                    $http .= "\r\n";

                    fwrite($sock, $http);

                    // Add to stream clients list
                    $key = array_search($sock, $read_socks);
                    $stream_clients[$key] = $options;
                    break;

                case '/':
                case '/status.xsl':
                    // Stream request
                    fwrite($sock, "HTTP/1.0 200 OK\r\n");
                    fwrite($sock, "Cache-Control: no-store, no-cache, must-revalidate\r\n");
                    fwrite($sock, "Content-Type: text/html\r\n");
                    fwrite($sock, "\r\n");

                    // Create xsl structure

                    // Header
                    $xsl = "";
                    $xsl .= "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" . "\n";
                    $xsl .= "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">" . "\n";
                    $xsl .= "<html xmlns=\"http://www.w3.org/1999/xhtml\">" . "\n";
                    $xsl .= "<head>" . "\n";
                    $xsl .= "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />" . "\n";
                    $xsl .= "<title>Icecast Streaming Media Server - Ampache</title>" . "\n";
                    $xsl .= "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />" . "\n";
                    $xsl .= "</head>" . "\n";
                    $xsl .= "<body>" . "\n";
                    $xsl .= "<div class=\"main\">" . "\n";

                    // Content
                    $xsl .= "<div class=\"roundcont\">" . "\n";
                    $xsl .= "<div class=\"roundtop\">" . "\n";
                    $xsl .= "<img src=\"images/corner_topleft.jpg\" class=\"corner\" style=\"display: none\" alt=\"\" />" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "<div class=\"newscontent\">" . "\n";
                    $xsl .= "<div class=\"streamheader\">" . "\n";
                    $xsl .= "<table cellspacing=\"0\" cellpadding=\"0\">" . "\n";
                    $xsl .= "<colgroup align=\"left\"></colgroup>" . "\n";
                    $xsl .= "<colgroup align=\"right\" width=\"300\"></colgroup>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td><h3>Mount Point /stream." . $channel->stream_type . "</h3></td>" . "\n";
                    $xsl .= "<td align=\"right\">" . "\n";
                    $xsl .= "<a href=\"stream.". $channel->stream_type .".m3u\">M3U</a>" . "\n";
                    $xsl .= "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "</table>" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "<table border=\"0\" cellpadding=\"4\">" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Stream Title:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $channel->name . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Stream Description:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $channel->description . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Content Type:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . Song::type_to_mime($channel->stream_type) . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Mount Start:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . date("c", $channel->start_date) . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Bitrate:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $channel->bitrate . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Current Listeners:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $channel->listeners . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Peak Listeners:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $channel->peak_listeners . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $genre = $channel->get_genre();
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Stream Genre:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $genre . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Stream URL:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\"><a href=\"" . $channel->url . "\" target=\"_blank\">" . $channel->url . "</a></td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $currentsong = "";
                    if ($channel->media) {
                        $currentsong = $channel->media->f_artist . " - " . $channel->media->f_title;
                    }
                    $xsl .= "<tr>" . "\n";
                    $xsl .= "<td>Current Song:</td>" . "\n";
                    $xsl .= "<td class=\"streamdata\">" . $currentsong . "</td>" . "\n";
                    $xsl .= "</tr>" . "\n";
                    $xsl .= "</table>" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "<div class=\"roundbottom\">" . "\n";
                    $xsl .= "<img src=\"images/corner_bottomleft.jpg\" class=\"corner\" style=\"display: none\" alt=\"\" />" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "<br /><br />" . "\n";

                    // Footer
                    $xsl .= "<div class=\"poster\">" . "\n";
                    $xsl .= "Support Ampache at <a target=\"_blank\" href=\"http://www.ampache.org\">www.ampache.org</a>" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "</div>" . "\n";
                    $xsl .= "</body>" . "\n";
                    $xsl .= "</html>" . "\n";

                    fwrite($sock, $xsl);

                    fclose($sock);
                    unset($client_socks[array_search($sock, $client_socks)]);
                    break;

                case '/style.css':
                case '/favicon.ico':
                case '/images/corner_bottomleft.jpg':
                case '/images/corner_bottomright.jpg':
                case '/images/corner_topleft.jpg':
                case '/images/corner_topright.jpg':
                case '/images/icecast.png':
                case '/images/key.png':
                case '/images/tunein.png':
                    // Get read file data
                    $fpath = AmpConfig::get('prefix') . '/channel' . $cmd['1'];
                    $pinfo = pathinfo($fpath);

                    $content_type = 'text/html';
                    switch ($pinfo['extension']) {
                        case 'css':
                            $content_type = "text/css";
                            break;
                        case 'jpg':
                            $content_type = "image/jpeg";
                            break;
                        case 'png':
                            $content_type = "image/png";
                            break;
                        case 'ico':
                            $content_type = "image/vnd.microsoft.icon";
                            break;
                    }
                    fwrite($sock, "HTTP/1.0 200 OK\r\n");
                    fwrite($sock, "Content-Type: " . $content_type . "\r\n");
                    $fdata = file_get_contents($fpath);
                    fwrite($sock, "Content-Length: " . strlen($fdata) . "\r\n");
                    fwrite($sock, "\r\n");
                    fwrite($sock, $fdata);
                    fclose($sock);
                    unset($client_socks[array_search($sock, $client_socks)]);
                    break;
                case '/stream.' . $channel->stream_type . '.m3u':
                    fwrite($sock, "HTTP/1.0 200 OK\r\n");
                    fwrite($sock, "Cache-control: public\r\n");
                    fwrite($sock, "Content-Disposition: filename=stream." . $channel->stream_type . ".m3u\r\n");
                    fwrite($sock, "Content-Type: audio/x-mpegurl\r\n");
                    fwrite($sock, "\r\n");

                    fwrite($sock, $channel->get_stream_url() . "\n");

                    fclose($sock);
                    unset($client_socks[array_search($sock, $client_socks)]);
                    break;
                default:
                    debug_event('channel', 'Unknown request. Closing connection.', '3');
                    fclose($sock);
                    unset($client_socks[array_search($sock, $client_socks)]);
                    break;
            }
        }
    }
}

function strtohex($x) {
    $s='';
    foreach(str_split($x) as $c) $s.=sprintf("%02X",ord($c));
    return($s);
}

?>
