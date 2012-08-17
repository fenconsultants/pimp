/*  Pimp firmware
    by Rob Voisey of Fen Consultants, UK
    
    Copyright (c) 2012, Fen Consultants
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
        * Redistributions of source code must retain the above copyright
          notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
          notice, this list of conditions and the following disclaimer in the
          documentation and/or other materials provided with the distribution.
        * Neither the name of Fen Consultants nor the names of its contributors
          may be used to endorse or promote products derived from this software
          without specific prior written permission.
    
    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL FEN CONSULTANTS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    
    Project repository at https://github.com/fenconsultants/pimp
    More information at http://fenconsultants.com/blog
*/

// Handles incoming data (cloud -> pimp)
class input extends InputPort
{
    id = null;

    constructor(i, name, type)
    {
        base.constructor(name, type);
        id = i;
    }
    
    // Override set to forward received data to the Pi
    function set(value)
    {
        // Send to Pi
        sendString(format("\xAA\x01%02d%s", id, value.tostring()));
    }
}

// Handles outgoing data (pimp -> cloud)
class output extends OutputPort
{
    id = null;

    constructor(i, name, type)
    {
        base.constructor(name, type);
        id = i;
    }
}

// Handles communication between Pi and imp
class raspberryPi
{
    // Hardware port for UART
    serPort = null;
    
    // Communication initialised flag
    initialised = false;
    
    // RX FSM state
    rxState = 0;
    rxCommand = 0;
    rxData = "";
    rxOffset = 0;
    
    // Ports
    inputPorts = [];
    outputPorts = [];

    constructor(port)
    {
        serPort = port;
        serPort.configure(115200, 8, PARITY_NONE, 1, NO_CTSRTS);
        initialise();
    }

    // Attempt to establish communication with Pi
    function initialise()
    {
        if(!initialised)
        {
            // Flush the serial port
            flush();

            // Send intialisation command
            sendString("\xAA\x00");

            // Wait up to 1s for response
            if(waitByte(1.0) == 0x55)
            {
                initialised = true;
                server.show("Connected");
                poll();
            }
            else
            {
                // If we can't connect, try again in a couple of seconds
                imp.wakeup(2.0, initialise.bindenv(this));
            }
        }
    }

    // Poll for data from Pi
    function poll()
    {
        // Empty Pi rx buffer into the FSM
        local c = serPort.read();
        while(c != -1)
        {
            rxFsm(c);
            c = serPort.read();
        }
        
        // Schedule next poll
        imp.wakeup(0.05, poll.bindenv(this));
    }

    // Process a byte received from Pi
    // TODO Timeout, and perhaps a CRC
    function rxFsm(c)
    {
        switch(rxState)
        {
            case 0 : // Waiting for header
                if(c == 0xAA) rxState = 1;
                break;
            
            case 1 : // Waiting for command
                switch(c)
                {
                    case 0x80 : // Define input port
                    case 0x81 : // Define output port
                    case 0x82 : // Output data
                        rxCommand = c;
                        rxState = 2;
                        break;

                    default: // Anything else is invalid
                        rxState = 0;
                        break;
                }
                break;
            
            case 2 : // Waiting for data length
                rxLength = c;
                rxData = "";
                rxOffset = 0;
                rxState = 3;
                break;
             
            case 3 : // Receiving data
                rxData += c.tochar();
                if(rxOffset == rxLength)
                {
                    // Data complete
                    command(rxCommand, rxData);
                    rxState = 0;
                    rxData = "";
                }
                break;
        }
    }

    // Process a command received from Pi
    function command(cmd, data = "")
    {
        switch(cmd)
        {
            case 0x80 : // Define input port
                local id = data.slice(0, 2).tointeger();
                local type = data.slice(2, data.find("\x09"));
                local name = data.slice(type.len()+3)
                inputPorts.append(input(id, name, type));
                server.log(format("Added input #%02d %s [%s]", id, name, type));
                imp.configure("Pimp", inputPorts, outputPorts);
                break;

            case 0x81 : // Define output port
                local id = data.slice(0, 2).tointeger();
                local type = data.slice(2, data.find("\x09"));
                local name = data.slice(type.len()+3)
                outputPorts.append(output(id, name, type));
                server.log(format("Added input #%02d %s [%s]", id, name, type));
                imp.configure("Pimp", inputPorts, outputPorts);
                break;

            case 0x82 : // Send to output
                local id = data.slice(0, 2).tointeger();
                local value = data.slice(2);
                sendOutput(id, value);
                break;

            default: // Anything else is invalid
                server.log(format("Invalid command 0x%02X", cmd));
                break;
        }
    }

    // Send data to the cloud (on all ports matching id)
    function sendOutput(id, value)
    {
        foreach(i, port in outputPorts)
            if(port.id == id)
                port.set(value);
    }

    // Flush any data waiting in the imp's serial rx buffer
    function flush()
    {
        while(serPort.read() != -1) ;
    }

    // Wait specified period (seconds) for a byte from the Pi
    function waitByte(t)
    {
        local tries = castf2i(t / 0.1);
        local c = serPort.read();
        while(c == -1 && --tries > 0)
        {
            imp.sleep(0.1);
            c = serPort.read();
        }
        return tries?c:-1;
    }

    // Send a string to the Pi
    function sendString(data)
    {
        foreach(i, val in data) serPort.write(val);
    }
}

// Initialise without ports, we'll add these when the Pi sends the config
imp.configure("Pimp", [], []);

// We'll boot faster than the Pi so give the user some planner feedback
server.show("Connecting with Pi...");

// Want pi now!
pi <- raspberryPi(hardware.uart12);

// End of code.
