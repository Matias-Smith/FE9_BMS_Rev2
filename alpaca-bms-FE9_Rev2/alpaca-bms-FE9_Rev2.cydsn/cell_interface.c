/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/

#include <cell_interface.h>
#include <LTC6811.h>
#include <math.h>
#include <stdlib.h>

//MUX Indexes:   
//First LTC on Node: 
//IC1_ADDRESSES: 0, 2, 4, 6, 8 
uint8_t CELL_TEMP_INDEXES_IC1[12] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};  //cell thermistors
uint8_t BOARD_TEMP_INDEXES_IC1[3] = {8, 10, 11};                                //board thermistors
uint8_t HUMIDITY_INDEX_IC1 = 9;                                               //humidity sensors
uint8_t HUMIDITY_INDEX_PACKS[1] = {0};                                          //subpacks w/ humidity

//Second LTC on Node: 
//IC2_ADDRESSES: 1, 3, 5, 7, 9 
uint8_t CELL_TEMP_INDEXES_IC2[12] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15};  //cell thermistors
uint8_t BOARD_TEMP_INDEXES_IC2[4] = {4, 5, 6, 7};                               //board thermistors

BAT_CELL_t bat_cell[N_OF_CELL];
BAT_TEMP_t bat_temp[N_OF_TEMP_CELL];
BAT_TEMP_t board_temp[N_OF_TEMP_BOARD];
PACK_HUMIDITY_t pack_humidity[N_OF_SUBPACK];
BAT_SUBPACK_t bat_subpack[N_OF_SUBPACK];
volatile BAT_ERR_t bat_err;
BAT_PACK_t bat_pack;
uint8_t spi_error_counter[N_OF_LTC];

void bms_init(uint8_t adc_mode){
    LTC6811_initialize(adc_mode);
}

//reads cell voltages from LTCs
//stores in bat_pack
void get_voltages(){
    SPI_ClearFIFO();
    
    LTC6811_adcv();  //run ADC conversion (all LTCs)
    CyDelay(1);
    
    uint16_t cell_voltages[CELLS_PER_LTC];  //temporarily store LTC voltages
    
    uint8_t addr; 
    for(addr = 0; addr < N_OF_LTC; addr++){
        if(LTC6811_rdcv_ltc(addr, cell_voltages)){
            spi_error_counter[addr]++;   //spi error
        } else {
            spi_error_counter[addr] = 0; //no spi error
        }
        
        //move voltages into bat_pack
        for(uint8_t cell = 0; cell < CELLS_PER_LTC; cell++){
            uint8_t subpack = addr/IC_PER_PACK;
            uint8_t ic_num = addr%IC_PER_PACK;
            uint8_t index = ic_num*CELLS_PER_LTC + cell;
            setVoltage(subpack, index, cell_voltages[cell]);
        }
    }
    
    CyDelay(1);
}

void get_temps(){
    SPI_ClearFIFO();
    
    uint16_t temps[N_OF_LTC][TEMPS_PER_LTC];   //store all temps in matrix
    uint8_t select;
    uint8_t ltc_addr;
    
    for(select = 0; select < TEMPS_PER_LTC; select++){       //for each mux pin
        for(ltc_addr = 0; ltc_addr < N_OF_LTC; ltc_addr++){  //for each LTC
            LTC6811_set_cfga_mux(ltc_addr, select);             //set tx_cfg variable
            LTC6811_wrcfga(ltc_addr);                           //write to LTC with wrcfga
        }
        
        LTC6811_adax(); //run ADC conversion (all LTCs)
        CyDelay(1);
        
        for(ltc_addr = 0; ltc_addr < N_OF_LTC; ltc_addr++){                    //for each LTC
            if(LTC6811_rdaux_pin(ltc_addr, GPIO5, &temps[ltc_addr][select])){  //get ADC result
                spi_error_counter[ltc_addr]++;  //spi error
            } else {
                spi_error_counter[ltc_addr] = 0;  //no spi error
            }
        }
    }
    
    //sort temps into bat_pack 
    for(uint8_t pack = 0; pack < N_OF_SUBPACK; pack++){
        uint8_t cell_temp_counter = 0;
        uint8_t board_temp_counter = 0; 
        uint8_t i;
        uint16_t raw_temp;
        uint16_t raw_humidity;
        
        ltc_addr = pack*IC_PER_PACK;
        //IC1 cell temps
        for(i = 0; i < sizeof(CELL_TEMP_INDEXES_IC1); i++){
            raw_temp = temps[ltc_addr][CELL_TEMP_INDEXES_IC1[i]];
            setCellTemp(pack, cell_temp_counter, raw_temp);
            cell_temp_counter++;
        }
        
        //IC1 board temps
        for(i = 0; i < sizeof(BOARD_TEMP_INDEXES_IC1); i++){
            raw_temp = temps[ltc_addr][BOARD_TEMP_INDEXES_IC1[i]];
            setBoardTemp(pack, board_temp_counter, raw_temp);
            board_temp_counter++;
        }
        //IC1 Humidity Sensor
        for(uint8_t i=0; i < sizeof(HUMIDITY_INDEX_PACKS);i++){ //see if pack matches hum_index_pack
            if (pack==HUMIDITY_INDEX_PACKS[i]){
                raw_humidity=temps[ltc_addr][HUMIDITY_INDEX_IC1];
                setBoardHum(pack,0,raw_humidity);
            }
        }
                
        ltc_addr++;
        //IC2 cell temps
        for(i = 0; i < sizeof(CELL_TEMP_INDEXES_IC2); i++){
            raw_temp = temps[ltc_addr][CELL_TEMP_INDEXES_IC2[i]];
            setCellTemp(pack, cell_temp_counter, raw_temp);
            cell_temp_counter++;
        }
        
        //IC2 board temps
        for(i = 0; i < sizeof(BOARD_TEMP_INDEXES_IC2); i++){
            raw_temp = temps[ltc_addr][BOARD_TEMP_INDEXES_IC2[i]];
            setBoardTemp(pack, board_temp_counter, raw_temp);
            board_temp_counter++;
        }
        
    }
    CyDelay(1); 
}

void check_voltages(){
    uint8_t cell, subpack;
    uint16_t voltage16 = 0;
    uint16_t max_voltage = 0;
    uint16_t min_voltage = 0xFFFF;

    // Check each cell
    for (cell = 0; cell < N_OF_CELL; cell++){
        //find max and min
        if (max_voltage < bat_cell[cell].voltage){
            max_voltage = bat_cell[cell].voltage;
        }
        if(min_voltage > bat_cell[cell].voltage){
            min_voltage = bat_cell[cell].voltage;
        }
        
        //check for errors
        voltage16 = bat_cell[cell].voltage;
        if (voltage16 > (uint16_t)OVER_VOLTAGE){
            bat_cell[cell].bad_counter++;
            bat_cell[cell].bad_type = 1;
        }else if (voltage16 < (uint16_t)UNDER_VOLTAGE){
            bat_cell[cell].bad_counter++;
            bat_cell[cell].bad_type = 0;
        }else{
            if (bat_cell[cell].bad_counter > 0){
                bat_cell[cell].bad_counter--;
            }           
        }
    }
    
    bat_pack.HI_voltage = max_voltage;
    bat_pack.LO_voltage = min_voltage;

    // Update subpacks & pack for errors
    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        for (cell = 0; cell < (N_OF_CELL / N_OF_SUBPACK); cell++){
            if (bat_subpack[subpack].cells[cell]->bad_counter > ERROR_VOLTAGE_LIMIT){
                if (bat_subpack[subpack].cells[cell]->bad_type == 0){
                    bat_subpack[subpack].under_voltage |= (1u<<cell);
                    bat_pack.status |= CELL_VOLT_OVER;
                    //bat_err_add(CELL_VOLT_OVER, bat_subpack[subpack].over_voltage, subpack);
                }else{
                    bat_subpack[subpack].over_voltage |= (1u<<cell);
                    bat_pack.status  |= CELL_VOLT_UNDER;
                    //bat_err_add(CELL_VOLT_UNDER, bat_subpack[subpack].under_voltage, subpack);
                }
            }
        }
    } 
    
    //check for spi errors
    for(uint8_t addr = 0; addr < N_OF_LTC; addr++){
        if(spi_error_counter[addr] > SPI_ERROR_LIMIT){
            bat_pack.status |= SPI_FAULT;         
            bat_pack.spi_error_address |= (1u<<addr);
        }
    }
}

void check_temps(){
    uint8_t temp, subpack, cell;
    uint16_t temp_c;
    
    //float median = getMedianTemp();
    float threshold = 2; // acceptable temperature degree threshold
    
    // check cell temps
    for (cell = 0; cell < N_OF_TEMP_CELL; cell++){
        temp_c = bat_temp[cell].temp_c;

        if (temp_c > (uint8_t)CRITICAL_TEMP_H){  //if over temperature
            bat_temp[cell].bad_counter++;
            bat_temp[cell].bad_type = 1;
        }else if (temp_c < (uint8_t)CRITICAL_TEMP_L){ // if under temperature
            bat_temp[cell].bad_counter++;
            bat_temp[cell].bad_type = 0;
        }else{    //if there is no error
            if (bat_temp[cell].bad_counter > 0){
                bat_temp[cell].bad_counter--;
            }           
        }
    }
    
#ifdef BALANCE_ON
    // check board temps
    for (cell = 0; cell < N_OF_TEMP_BOARD; cell++){
        temp_c = board_temp[cell].temp_c;
        if (temp_c > (uint8_t)CRITICAL_TEMP_BOARD_H){
            //if over temperature
            board_temp[cell].bad_counter++;
            board_temp[cell].bad_type = 1;
        }else if (temp_c < (uint8_t)CRITICAL_TEMP_BOARD_L){
            // if under temperature
            board_temp[cell].bad_counter++;
            board_temp[cell].bad_type = 0;
        }else{
            //if there is no error
            if (board_temp[cell].bad_counter > 0){
                board_temp[cell].bad_counter--;
            }           
        }
    }
#endif
   
    // Update subpacks
    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        for (temp = 0; temp < (CELL_TEMPS_PER_PACK); temp++){
            if (bat_subpack[subpack].cell_temps[temp]->bad_counter > ERROR_TEMPERATURE_LIMIT){
                if (bat_subpack[subpack].cell_temps[temp]->bad_type == 0){
                    bat_subpack[subpack].under_temp_cell |= (1u<<temp);
                }else{
                    bat_subpack[subpack].over_temp_cell |= (1u<<temp);
                }
            }
        }
        for (temp = 0; temp < (BOARD_TEMPS_PER_PACK); temp++){
            if (bat_subpack[subpack].board_temps[temp]->bad_counter > ERROR_TEMPERATURE_LIMIT){
                if (bat_subpack[subpack].board_temps[temp]->bad_type == 0){
                    bat_subpack[subpack].under_temp_board |= (1u<<temp);
                }else{
                    bat_subpack[subpack].over_temp_board |= (1u<<temp);
                }
            }
        }
    }
    
    // update max temps
    uint8_t max_temp;
    uint8_t i;
    
    bat_pack.HI_temp_c = 0;
    bat_pack.HI_temp_raw = 0;
    
    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        max_temp = bat_pack.subpacks[subpack]->cell_temps[0]->temp_c;
        for (i = 0; i < (CELL_TEMPS_PER_PACK); i++){
            temp = bat_pack.subpacks[subpack]->cell_temps[i]->temp_c;
            
            //update subpack max temp
            if (max_temp < temp){
                max_temp = bat_pack.subpacks[subpack]->cell_temps[i]->temp_c;
            }
            
            //update pack max temp
            if (temp > bat_pack.HI_temp_c){
                bat_pack.HI_temp_c = temp;
                bat_pack.HI_temp_raw = bat_pack.subpacks[subpack]->cell_temps[i]->temp_raw;
                bat_pack.HI_temp_subpack = subpack;
                bat_pack.HI_temp_subpack_index = i;
            } 
        }
        
        bat_pack.subpacks[subpack]->high_temp = max_temp;
    }

#ifdef BALANCE_ON
    // Update the battery_pack highest temperature
    bat_pack.HI_temp_board_c = board_temp[0].temp_c;
    bat_pack.HI_temp_board_node = 0;
    
    for (i = 1; i < N_OF_TEMP_BOARD; i++){
        if (board_temp[i].temp_c > bat_pack.HI_temp_board_c){
            bat_pack.HI_temp_board_c = board_temp[i].temp_c;
            bat_pack.HI_temp_board_node = i / (N_OF_TEMP_BOARD / N_OF_SUBPACK);
        }    
    }
#endif

    // update pack of temp error
    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        if (bat_pack.subpacks[subpack]->over_temp_cell != 0){
            bat_pack.status |= PACK_TEMP_OVER;
            //bat_err_add(PACK_TEMP_OVER, bat_subpack[subpack].over_temp, subpack);  
        }

        if (bat_pack.subpacks[subpack]->under_temp_cell != 0){
            bat_pack.status  |= PACK_TEMP_UNDER;
            //bat_err_add(PACK_TEMP_UNDER, bat_subpack[subpack].under_temp, subpack);
        }
        
        if (bat_pack.subpacks[subpack]->over_temp_board != 0){
            bat_pack.status |= PACK_TEMP_OVER;
            //bat_err_add(PACK_TEMP_OVER, bat_subpack[subpack].over_temp, subpack);
        }

        if (bat_pack.subpacks[subpack]->under_temp_board != 0){
            bat_pack.status  |= PACK_TEMP_UNDER;
            //bat_err_add(PACK_TEMP_UNDER, bat_subpack[subpack].under_temp, subpack);
        }
    }
    
}

/**
 * @initialize the mypack struct. 
 *
 * @param no input parameters.
 * @return 1 if everything is OK. 0 for hard failure.
 */
void mypack_init(){
    
    uint8_t cell = 0;
    uint8_t subpack = 0;
    uint8_t temp = 0;
    //bat_err_index = 0;
    //bat_err_index_loop = 0;

    // initialize cells and temps
    
    for (cell = 0; cell < N_OF_CELL; cell++){
        bat_cell[cell].voltage = 0;
        bat_cell[cell].bad_counter = 0;
    }
    
    for (temp = 0; temp < N_OF_TEMP_CELL; temp++){
        bat_temp[temp].temp_c = (uint8_t)temp;
        bat_temp[temp].temp_raw = (uint16_t)temp;
        bat_temp[temp].bad_counter = 0;
        bat_temp[temp].bad_type = 0;
        bat_temp[temp].type = THERM_CELL;
    }
    for (temp = 0; temp < N_OF_TEMP_BOARD; temp++){
        board_temp[temp].temp_c = (uint8_t)temp;
        board_temp[temp].temp_raw = (uint16_t)temp;
        board_temp[temp].bad_counter = 0;
        board_temp[temp].bad_type = 0;
        board_temp[temp].type = THERM_BOARD;
    }
    
    //initialize humidity values
    for (uint8_t i=0; i<N_OF_SUBPACK;i++){
        pack_humidity[i].humidity_raw=0;
        pack_humidity[i].humidity=0;
    }
    
    //initialize spi error counter
    for (uint8_t addr = 0; addr < N_OF_LTC; addr++){
        spi_error_counter[addr] = 0;
    }

    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        for (cell = 0; cell < (CELLS_PER_SUBPACK); cell++){    
            bat_subpack[subpack].cells[cell] = &(bat_cell[subpack*(CELLS_PER_SUBPACK)+cell]);
        }
        for (temp = 0; temp < (CELL_TEMPS_PER_PACK); temp++){
            bat_subpack[subpack].cell_temps[temp] = &(bat_temp[subpack*(CELL_TEMPS_PER_PACK)+temp]);
        }
        for (temp = 0; temp < (BOARD_TEMPS_PER_PACK); temp++){
            bat_subpack[subpack].board_temps[temp] = &(board_temp[subpack*(BOARD_TEMPS_PER_PACK)+temp]);
        }
        bat_subpack[subpack].pack_humidity[0]= &pack_humidity[subpack];
        
        bat_subpack[subpack].over_temp_cell = 0;
        bat_subpack[subpack].under_temp_cell = 0;
        bat_subpack[subpack].over_voltage = 0;
        bat_subpack[subpack].under_voltage = 0;
        bat_subpack[subpack].voltage = 0;
    }
    
    // Register pack
    for (subpack = 0; subpack < N_OF_SUBPACK; subpack++){
        bat_pack.subpacks[subpack] = &(bat_subpack[subpack]);
    }
    
    //initialize pack values
    bat_pack.voltage = 0;
    bat_pack.current = 0;
    bat_pack.fuse_fault = 0;
    bat_pack.status = 0; 
    bat_pack.health = NORMAL;
    bat_pack.SOC_cali_flag =0;
    bat_pack.HI_temp_c = 0;
    bat_pack.HI_temp_raw = 0;
    bat_pack.HI_voltage = 0;
    bat_pack.LO_voltage = 0;
    bat_pack.time_stamp = 0;
    bat_pack.SOC_percent = 0;
}

void setVoltage(uint8_t pack, uint8_t index, uint16_t raw_voltage){
    bat_pack.subpacks[pack]->cells[index]->voltage = raw_voltage;
}

void setCellTemp(uint8_t pack, uint8_t index, uint16_t raw_temp){
    bat_pack.subpacks[pack]->cell_temps[index]->temp_raw = raw_temp;
    bat_pack.subpacks[pack]->cell_temps[index]->temp_c = rawToCelcius(raw_temp);
}

void setBoardTemp(uint8_t pack, uint8_t index, uint16_t raw_temp){
    bat_pack.subpacks[pack]->board_temps[index]->temp_raw = raw_temp;
    bat_pack.subpacks[pack]->board_temps[index]->temp_c = rawToCelcius(raw_temp);
}

void setBoardHum(uint8_t pack, uint8_t index, uint16_t raw_humidity){
    bat_pack.subpacks[pack]->pack_humidity[index]->humidity_raw = raw_humidity;
    bat_pack.subpacks[pack]->pack_humidity[index]->humidity = rawToHumidity(raw_humidity);
  //still need to create rawToRelative function for conversion
}
float32 rawToCelcius(uint16_t raw){
    float32 temp = (float32)raw/10000;
    temp = (1/((1/298.15) + ((1/3428.0)*log(temp/(3-temp))))) - 273.15;
    
    return temp;
}

uint8_t rawToHumidity(uint16_t raw){
    double humidity = (double)raw/10000;
    humidity = ((humidity/3.0f)-0.1515f)/0.00636f;
    return (uint8_t)humidity;
}

/* [] END OF FILE */
